// ---------------------------------------------------------------------------
// Walk-test receiver — runs on the gateway node (plain XIAO ESP32S3 + B2B kit).
//
// Listens for pings, ACKs each one with the RSSI/SNR it measured, and prints a
// CSV line per packet plus a rolling summary. Sits on USB next to the HA host
// during the test; MQTT comes later.
//
// Sequence gaps are counted so packet delivery ratio is measured at the
// receiver too, not just inferred from missing ACKs at the sender.
// ---------------------------------------------------------------------------
#include <Arduino.h>
#include "board_pins.h"
#include "link_config.h"
#include "lora_radio.h"

static uint32_t received = 0, missed = 0, badFrames = 0;
static uint16_t lastSeq = 0;
static bool haveLastSeq = false;
static float rssiMin = 0, rssiMax = -200, rssiSum = 0;
static uint32_t lastReportMs = 0;

// Polled rather than interrupt-driven; see the note in tx_main.cpp for why
// attachInterrupt() panics the ipc1 task on this platform.
static inline bool packetReady() {
    // DIO1 alone is not enough: the SX126x shares one buffer between TX and RX,
    // so a leftover TxDone assertion makes readData() hand back the packet we
    // just sent. Confirm the RxDone flag before trusting the pin.
    if (digitalRead(LORA_DIO1) != HIGH) return false;
    return (radio.getIrqFlags() & (1UL << RADIOLIB_IRQ_RX_DONE)) != 0;
}

static void blink(uint16_t ms) {
    digitalWrite(USER_LED, LED_ON);
    delay(ms);
    digitalWrite(USER_LED, LED_OFF);
}

void setup() {
    Serial.begin(115200);
    pinMode(USER_LED, OUTPUT);
    digitalWrite(USER_LED, LED_OFF);

    uint32_t t0 = millis();
    while (!Serial && millis() - t0 < 3000) delay(10);

    Serial.println("\n=== LoRa walk test :: gateway node (RX) ===");

    if (loraBegin() != RADIOLIB_ERR_NONE) {
        Serial.println("[fatal] radio init failed; halting. Check board_pins.h against your module.");
        while (true) { blink(60); delay(400); }
    }

    pinMode(LORA_DIO1, INPUT);
    int16_t state = radio.startReceive();
    if (state != RADIOLIB_ERR_NONE) {
        Serial.printf("[fatal] startReceive failed: %d (%s)\n", state, loraErrName(state));
        while (true) { blink(60); delay(400); }
    }

    Serial.println("listening...");
    Serial.println("seq,rssi,snr,freq_err,vbat_mv,rx_ok,missed,pdr%");
    lastReportMs = millis();
}

void loop() {
    if (packetReady()) {
        // Two message types share the air: walk-test pings and meter readings.
        // Read into a buffer big enough for either and dispatch on the magic byte.
        uint8_t raw[sizeof(ReadingPacket)] = {};
        size_t avail = radio.getPacketLength();
        int16_t state = radio.readData(raw, sizeof(raw));
        PingPacket ping = {};
        ReadingPacket rd = {};
        bool isReading = (avail == sizeof(ReadingPacket) && raw[0] == MSG_MAGIC_READ);
        if (isReading) memcpy(&rd, raw, sizeof(rd));
        else           memcpy(&ping, raw, min(avail, sizeof(ping)));

        // Capture link metrics before touching the radio again.
        float rssi = radio.getRSSI();
        float snr = radio.getSNR();
        float ferr = radio.getFrequencyError();

        if (state == RADIOLIB_ERR_NONE && isReading && rd.version == MSG_VERSION) {
            // A decoded meter reading. Value travels as tenths of a kWh.
            received++;
            Serial.printf("READING,%u,%lu,%u,%u,%.1f,%.1f,%u,%u\n",
                          rd.seq, (unsigned long)rd.value, rd.digits,
                          rd.confidence, rssi, snr, rd.vbat_mv,
                          (rd.flags & READING_FLAG_DECODE_OK) ? 1 : 0);

            delay(ACK_TURNAROUND_MS);
            AckPacket ack = {};
            ack.magic = MSG_MAGIC_ACK;
            ack.version = MSG_VERSION;
            ack.seq = rd.seq;
            ack.rssi_dbm = (int16_t)lroundf(rssi);
            ack.snr_ddb = (int16_t)lroundf(snr * 10.0f);
            radio.transmit((uint8_t *)&ack, sizeof(ack));
            radio.clearIrqFlags(0xFFFFFFFFUL);
            blink(20);

        } else if (state == RADIOLIB_ERR_NONE && ping.magic == MSG_MAGIC_PING &&
            ping.version == MSG_VERSION) {

            received++;
            if (haveLastSeq) {
                uint16_t gap = (uint16_t)(ping.seq - lastSeq);
                if (gap > 1) missed += (gap - 1);   // wrap-safe
            }
            lastSeq = ping.seq;
            haveLastSeq = true;

            if (rssi < rssiMin || received == 1) rssiMin = rssi;
            if (rssi > rssiMax) rssiMax = rssi;
            rssiSum += rssi;

            uint32_t total = received + missed;
            float pdr = total ? (100.0f * received / total) : 0.0f;

            Serial.printf("%u,%.1f,%.1f,%.0f,%u,%lu,%lu,%.1f\n",
                          ping.seq, rssi, snr, ferr, ping.vbat_mv,
                          (unsigned long)received, (unsigned long)missed, pdr);

            // Give the sender time to switch from TX to RX before replying.
            delay(ACK_TURNAROUND_MS);

            AckPacket ack = {};
            ack.magic = MSG_MAGIC_ACK;
            ack.version = MSG_VERSION;
            ack.seq = ping.seq;
            ack.rssi_dbm = (int16_t)lroundf(rssi);
            ack.snr_ddb = (int16_t)lroundf(snr * 10.0f);

            int16_t txState = radio.transmit((uint8_t *)&ack, sizeof(ack));
            if (txState != RADIOLIB_ERR_NONE) {
                Serial.printf("[warn] ACK transmit failed: %d (%s)\n",
                              txState, loraErrName(txState));
            }
            radio.clearIrqFlags(0xFFFFFFFFUL);  // drop TxDone from our own ACK
            blink(20);
        } else if (state != RADIOLIB_ERR_NONE) {
            badFrames++;
            Serial.printf("[warn] rx error %d (%s)\n", state, loraErrName(state));
        } else {
            badFrames++;
            Serial.printf("[warn] unknown frame: magic=0x%02X ver=%u rssi=%.1f\n",
                          ping.magic, ping.version, rssi);
        }

        radio.startReceive();
    }

    // Rolling summary every 30 s so the test is readable without post-processing.
    if (millis() - lastReportMs >= 30000) {
        lastReportMs = millis();
        uint32_t total = received + missed;
        Serial.printf("[summary] rx=%lu missed=%lu bad=%lu pdr=%.1f%% "
                      "rssi min/avg/max = %.1f/%.1f/%.1f dBm\n",
                      (unsigned long)received, (unsigned long)missed,
                      (unsigned long)badFrames,
                      total ? (100.0f * received / total) : 0.0f,
                      received ? rssiMin : 0.0f,
                      received ? rssiSum / received : 0.0f,
                      received ? rssiMax : 0.0f);
    }
}
