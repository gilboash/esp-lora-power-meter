// ---------------------------------------------------------------------------
// Optical pulse node: count the meter's 500 imp/kWh LED and report the total.
//
// The third variant. The camera node reads the printed digits; this one reads
// the meter's own optical output, which is simpler and far more robust: no
// lighting, no glare through two layers of plastic, no OCR. It gives increments
// rather than an absolute reading, which Home Assistant's utility_meter turns
// into kWh from a one-time seed of whatever the meter currently shows.
//
// Hardware: plain XIAO ESP32S3 + pin-header Wio-SX1262, TEPT4400 on D0.
//
// Counting is done by the ESP32's PCNT peripheral rather than in software.
// That is not premature optimisation -- a transmit at SF12 blocks this task for
// well over a second, and polling a pin would simply miss any pulse that landed
// in that window. For a cumulative meter a missed pulse is a permanent error,
// so the counter has to run independently of what the CPU is doing.
// ---------------------------------------------------------------------------
#include <Arduino.h>
#include <Preferences.h>
#include <driver/pulse_cnt.h>
#include "board_pins.h"
#include "link_config.h"
#include "lora_radio.h"

// PCNT counts to this before wrapping; we drain it far more often than that.
static const int PCNT_HIGH_LIMIT = 10000;

static pcnt_unit_handle_t pcntUnit = nullptr;
static pcnt_channel_handle_t pcntChan = nullptr;

static Preferences prefs;
struct PulseConfig {
    uint32_t total;        // cumulative pulses, persisted so a reboot is not a reset
    uint16_t imp_kwh;      // 500 for this meter; printed on the faceplate
    uint16_t report_secs;  // how often to transmit
};
static PulseConfig CFG = { 0, 500, 60 };

static uint16_t txSeq = 0;
static uint32_t lastTotalSent = 0;
static uint32_t lastReportMs = 0;
static bool radioUp = false;
static uint32_t ackTimeoutMs = ACK_TIMEOUT_MS;

// ---------------------------------------------------------------------------

static void cfgLoad() {
    prefs.begin("pulse", true);
    CFG.total       = prefs.getULong("tot", 0);
    CFG.imp_kwh     = prefs.getUShort("imp", 500);
    CFG.report_secs = prefs.getUShort("rep", 60);
    prefs.end();
}

static void cfgSave() {
    prefs.begin("pulse", false);
    prefs.putULong("tot", CFG.total);
    prefs.putUShort("imp", CFG.imp_kwh);
    prefs.putUShort("rep", CFG.report_secs);
    prefs.end();
}

static void cfgPrint() {
    Serial.printf("[cfg] total=%lu imp_kwh=%u report=%us\n",
                  (unsigned long)CFG.total, CFG.imp_kwh, CFG.report_secs);
}

static bool pulseBegin() {
    pcnt_unit_config_t unitCfg = {};
    unitCfg.high_limit = PCNT_HIGH_LIMIT;
    unitCfg.low_limit = -1;
    if (pcnt_new_unit(&unitCfg, &pcntUnit) != ESP_OK) return false;

    // Reject anything shorter than 10 us. The hardware filter counts APB clock
    // cycles and tops out around 12 us, so this is near its ceiling -- asking
    // for 1 ms is rejected outright.
    //
    // It only removes electrical pickup on the sensor lead. It cannot help with
    // the thing that actually causes false counts: mains-powered room lighting
    // flickering at 100 Hz, which is real light of a plausible duration. The
    // bench proved the point by counting 14 "pulses" from the ceiling light.
    // Shrouding is the fix; see the saturation warning below.
    pcnt_glitch_filter_config_t filter = {};
    filter.max_glitch_ns = 10 * 1000;
    if (pcnt_unit_set_glitch_filter(pcntUnit, &filter) != ESP_OK) {
        // Older silicon limits the filter window; carry on without it rather
        // than refuse to count at all.
        Serial.println("[pcnt] glitch filter rejected, continuing without");
    }

    pcnt_chan_config_t chanCfg = {};
    chanCfg.edge_gpio_num = PULSE_PIN;
    chanCfg.level_gpio_num = -1;
    if (pcnt_new_channel(pcntUnit, &chanCfg, &pcntChan) != ESP_OK) return false;

    // The phototransistor pulls the pin DOWN when lit, so a pulse is a falling
    // edge. See the wiring note in board_pins.h.
    pcnt_channel_set_edge_action(pcntChan,
                                 PCNT_CHANNEL_EDGE_ACTION_INCREASE,  // falling
                                 PCNT_CHANNEL_EDGE_ACTION_HOLD);     // rising

    // The internal pull-up is the sensor's load resistor; without it the pin
    // floats between pulses and counts noise.
    pinMode(PULSE_PIN, INPUT_PULLUP);

    if (pcnt_unit_enable(pcntUnit) != ESP_OK) return false;
    if (pcnt_unit_clear_count(pcntUnit) != ESP_OK) return false;
    if (pcnt_unit_start(pcntUnit) != ESP_OK) return false;
    return true;
}

// Move whatever the hardware has counted into the running total.
static uint32_t drainCounter() {
    int count = 0;
    if (!pcntUnit || pcnt_unit_get_count(pcntUnit, &count) != ESP_OK) return 0;
    if (count <= 0) return 0;
    pcnt_unit_clear_count(pcntUnit);
    CFG.total += (uint32_t)count;
    return (uint32_t)count;
}

static uint16_t readBatteryMv() {
#if HAS_BATTERY_DIVIDER
    uint32_t acc = 0;
    for (int i = 0; i < 8; i++) acc += analogReadMilliVolts(VBAT_ADC_PIN);
    return (uint16_t)((acc / 8) * VBAT_DIVIDER);
#else
    return 0;
#endif
}

static inline bool packetReady() {
    if (digitalRead(LORA_DIO1) != HIGH) return false;
    return (radio.getIrqFlags() & (1UL << RADIOLIB_IRQ_RX_DONE)) != 0;
}

static void blinkLed(uint8_t times, uint16_t onMs, uint16_t offMs) {
    for (uint8_t i = 0; i < times; i++) {
        digitalWrite(USER_LED, LED_ON);
        delay(onMs);
        digitalWrite(USER_LED, LED_OFF);
        if (i + 1 < times) delay(offMs);
    }
}

static bool sendPulses(uint32_t total, uint16_t delta, uint16_t secs) {
    PulsePacket pkt = {};
    pkt.magic = MSG_MAGIC_PULSE;
    pkt.version = MSG_VERSION;
    pkt.seq = ++txSeq;
    pkt.total = total;
    pkt.delta = delta;
    pkt.secs = secs;
    pkt.imp_kwh = CFG.imp_kwh;
    pkt.vbat_mv = readBatteryMv();

    for (int attempt = 0; attempt < 3; attempt++) {
        if (radio.transmit((uint8_t *)&pkt, sizeof(pkt)) != RADIOLIB_ERR_NONE) continue;
        radio.clearIrqFlags(0xFFFFFFFFUL);
        radio.startReceive();
        uint32_t t0 = millis();
        while (millis() - t0 < ackTimeoutMs) {
            if (packetReady()) {
                AckPacket ack = {};
                if (radio.readData((uint8_t *)&ack, sizeof(ack)) == RADIOLIB_ERR_NONE &&
                    ack.magic == MSG_MAGIC_ACK && ack.seq == pkt.seq) {
                    radio.standby();
                    return true;
                }
                radio.startReceive();
            }
            delay(1);
        }
        radio.standby();
        delay(200);
    }
    return false;
}

static void handleCommandLine(const String &line) {
    if (line.startsWith("SET ")) {
        int sp = line.indexOf(' ', 4);
        if (sp < 0) return;
        String key = line.substring(4, sp);
        long v = line.substring(sp + 1).toInt();
        if      (key == "imp")    CFG.imp_kwh = constrain(v, 1, 10000);
        else if (key == "report") CFG.report_secs = constrain(v, 5, 3600);
        // Seeding: set the running total to match the meter's own display, so
        // the two agree from this point on. The sent-marker has to move with
        // it, or the next delta is computed against a stale baseline and
        // underflows into a nonsense value.
        else if (key == "total") {
            CFG.total = (uint32_t)max(0L, v);
            lastTotalSent = CFG.total;
        }
        else { Serial.println("[cfg] unknown key"); return; }
        cfgSave();
        cfgPrint();
    } else if (line == "SEND") {
        uint32_t d = drainCounter();
        Serial.printf("[tx] %s\n", sendPulses(CFG.total, (uint16_t)d, 0) ? "acked" : "no ack");
        cfgSave();
    } else if (line == "RAW") {
        // Live pin level, for checking sensor polarity and aim without a meter.
        Serial.printf("[raw] D0=%d  adc=%u mV\n",
                      digitalRead(PULSE_PIN), analogReadMilliVolts(PULSE_PIN));
    }
}

static void pollCommandLines() {
    static String line;
    while (Serial.available()) {
        char ch = (char)Serial.read();
        if (ch == '\n' || ch == '\r') {
            if (line.length()) { handleCommandLine(line); line = ""; }
        } else if (line.length() < 64) {
            line += ch;
        }
    }
}

void setup() {
    Serial.begin(115200);
    pinMode(USER_LED, OUTPUT);
    digitalWrite(USER_LED, LED_OFF);
    uint32_t t0 = millis();
    while (!Serial && millis() - t0 < 2000) delay(10);

    Serial.println("\n=== pulse node :: optical meter counter ===");
    cfgLoad();
    cfgPrint();
    Serial.println("Commands: SET imp|report|total <n> | SEND | RAW");

    if (!pulseBegin()) {
        Serial.println("[fatal] PCNT init failed");
    } else {
        Serial.printf("[pcnt] counting falling edges on GPIO%d\n", PULSE_PIN);
    }

    radioUp = (loraBegin() == RADIOLIB_ERR_NONE);
    if (!radioUp) {
        Serial.println("[warn] radio init failed; will retry while running");
    } else {
        pinMode(LORA_DIO1, INPUT);
        ackTimeoutMs = loraTimeOnAirMs(sizeof(AckPacket)) + ACK_TURNAROUND_MS + 400;
        if (ackTimeoutMs < ACK_TIMEOUT_MS) ackTimeoutMs = ACK_TIMEOUT_MS;
        Serial.printf("[radio] ack window %lu ms\n", (unsigned long)ackTimeoutMs);
    }
    blinkLed(1, 60, 0);
    lastReportMs = millis();
    lastTotalSent = CFG.total;
}

void loop() {
    pollCommandLines();
    const uint32_t now = millis();

    // Re-announce periodically: the dashboard is usually started long after the
    // node booted, and with a dark sensor there is otherwise nothing in the
    // output for it to recognise this node by.
    static uint32_t lastCfgPrint = 0;
    if (Serial && now - lastCfgPrint > 10000) { lastCfgPrint = now; cfgPrint(); }

    // Drain the hardware counter often so a pulse shows up in the log promptly,
    // and so the count cannot approach the wrap limit between reports.
    static uint32_t lastSeenTotal = 0;
    uint32_t got = drainCounter();
    if (got) {
        Serial.printf("[pulse] +%lu  total=%lu  (%.3f kWh)\n",
                      (unsigned long)got, (unsigned long)CFG.total,
                      (double)CFG.total / CFG.imp_kwh);
        blinkLed(1, 25, 0);          // visible confirmation at the meter
        lastSeenTotal = CFG.total;
    }

    // Ambient light check. If the pin sits low continuously the phototransistor
    // is saturated by something other than the meter's LED, and every count is
    // suspect. Worth saying out loud during installation, when the sensor is
    // being positioned and the shroud is easy to get wrong.
    static uint32_t lowSince = 0;
    static uint32_t lastSatWarn = 0;
    if (digitalRead(PULSE_PIN) == LOW) {
        if (!lowSince) lowSince = now;
        if (now - lowSince > 3000 && now - lastSatWarn > 15000) {
            lastSatWarn = now;
            Serial.println("[warn] sensor saturated - held low for >3 s. "
                           "Stray light, not a pulse. Shroud it.");
        }
    } else {
        lowSince = 0;
    }

    // A node that gives up on its radio for good is indistinguishable from a
    // flat battery once installed, so keep retrying.
    static uint32_t lastRadioTry = 0;
    if (!radioUp && now - lastRadioTry >= 5000) {
        lastRadioTry = now;
        radioUp = (loraBegin() == RADIOLIB_ERR_NONE);
        if (radioUp) {
            pinMode(LORA_DIO1, INPUT);
            ackTimeoutMs = loraTimeOnAirMs(sizeof(AckPacket)) + ACK_TURNAROUND_MS + 400;
            if (ackTimeoutMs < ACK_TIMEOUT_MS) ackTimeoutMs = ACK_TIMEOUT_MS;
            Serial.println("[radio] recovered");
        }
    }

    if (radioUp && now - lastReportMs >= (uint32_t)CFG.report_secs * 1000) {
        uint16_t secs = (uint16_t)((now - lastReportMs) / 1000);
        lastReportMs = now;
        // Unsigned subtraction: if the total was reseeded below the marker this
        // would wrap to billions, so treat any decrease as "nothing new".
        uint32_t delta32 = (CFG.total >= lastTotalSent) ? CFG.total - lastTotalSent : 0;
        uint16_t delta = delta32 > 0xFFFF ? 0xFFFF : (uint16_t)delta32;

        // Sanity-check the implied load. A 3-phase 100 A supply tops out near
        // 69 kW, so anything beyond that is not consumption -- it is stray
        // light being counted. The count cannot be un-made, but it must be
        // visible rather than silently inflating a cumulative total.
        if (secs && CFG.imp_kwh) {
            float watts = ((float)delta / CFG.imp_kwh) * 1000.0f * 3600.0f / secs;
            if (watts > 70000.0f) {
                Serial.printf("[warn] %.0f W implied - impossible. The sensor is "
                              "seeing stray light; shroud it.\n", watts);
            }
        }

        // Always report, even with no pulses: zero consumption and a dead node
        // must not look the same from upstairs.
        bool ok = sendPulses(CFG.total, delta, secs);
        Serial.printf("[tx] seq=%u total=%lu delta=%u over %us -> %s\n",
                      txSeq, (unsigned long)CFG.total, delta, secs,
                      ok ? "acked" : "no ack");
        if (ok) {
            lastTotalSent = CFG.total;
            blinkLed(2, 40, 120);
        } else {
            blinkLed(1, 400, 0);
        }
        // Persist after each report rather than on every pulse: NVS has a write
        // endurance, and losing at most one reporting interval on a power cut
        // is a fair trade for not wearing the flash out.
        cfgSave();
    }

    delay(5);
}
