// ---------------------------------------------------------------------------
// Combined camera node: LoRa walk-test pings AND USB camera streaming.
//
// The two are independent -- the camera sits on the DVP pins (GPIO 10-18, 38-48)
// and the SX1262 on GPIO 2-9 -- so both run from one sketch. This is the bench
// and install configuration: you can watch the live view while confirming the
// radio link still holds, from the same dashboard.
//
// Frames go out over USB only. LoRa carries just the ping (and later the
// decoded reading): at 1% duty cycle the radio moves about 2 bytes a second,
// which is nowhere near enough for imagery.
//
// The LED still reports link quality for walking the building:
//   1 blink strong · 2 usable · 3 marginal · 1 long blink no ACK
// ---------------------------------------------------------------------------
#include <Arduino.h>
#include "board_pins.h"
#include "link_config.h"
#include "lora_radio.h"
#include "camera_hw.h"

static uint16_t seq = 0;
static uint32_t sent = 0, acked = 0;
static uint32_t pingIntervalMs = PING_INTERVAL_MS;
static uint32_t lastPing = 0;
static bool radioUp = false;

// Polled rather than interrupt-driven: RadioLib's setPacketReceivedAction()
// calls attachInterrupt(), and ESP-IDF routes GPIO ISR registration through the
// ipc1 task, whose ~1 kB stack overflows in esp_intr_alloc(). Also confirm the
// RxDone flag -- the SX126x shares one buffer between TX and RX, so a leftover
// TxDone assertion would hand back the packet we just sent.
static inline bool packetReady() {
    if (digitalRead(LORA_DIO1) != HIGH) return false;
    return (radio.getIrqFlags() & (1UL << RADIOLIB_IRQ_RX_DONE)) != 0;
}

static void blink(uint8_t times, uint16_t onMs, uint16_t offMs) {
    for (uint8_t i = 0; i < times; i++) {
        digitalWrite(USER_LED, LED_ON);
        delay(onMs);
        digitalWrite(USER_LED, LED_OFF);
        if (i + 1 < times) delay(offMs);
    }
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

static void sendPing() {
    PingPacket ping = {};
    ping.magic = MSG_MAGIC_PING;
    ping.version = MSG_VERSION;
    ping.seq = seq;
    ping.vbat_mv = readBatteryMv();

    int16_t state = radio.transmit((uint8_t *)&ping, sizeof(ping));
    bool txOk = (state == RADIOLIB_ERR_NONE);
    if (txOk) sent++;

    bool gotAck = false;
    int16_t upRssi = 0, upSnrDdb = 0;
    float dnRssi = 0, dnSnr = 0;

    if (txOk) {
        radio.clearIrqFlags(0xFFFFFFFFUL);  // drop TxDone before listening
        radio.startReceive();
        uint32_t waitStart = millis();
        while (millis() - waitStart < ACK_TIMEOUT_MS) {
            if (packetReady()) {
                AckPacket ack = {};
                int16_t rd = radio.readData((uint8_t *)&ack, sizeof(ack));
                if (rd == RADIOLIB_ERR_NONE && ack.magic == MSG_MAGIC_ACK &&
                    ack.version == MSG_VERSION && ack.seq == ping.seq) {
                    gotAck = true;
                    upRssi = ack.rssi_dbm;
                    upSnrDdb = ack.snr_ddb;
                    dnRssi = radio.getRSSI();
                    dnSnr = radio.getSNR();
                    break;
                }
                radio.startReceive();
            }
            delay(1);
        }
        radio.standby();
    }

    if (gotAck) acked++;
    float pdr = sent ? (100.0f * acked / sent) : 0.0f;

    if (gotAck) {
        Serial.printf("%u,%d,1,%d,%.1f,%.1f,%.1f,%.1f,%u\n",
                      ping.seq, txOk ? 1 : 0, upRssi, upSnrDdb / 10.0f,
                      dnRssi, dnSnr, pdr, ping.vbat_mv);
        if (upRssi > -100)      blink(1, 40, 120);
        else if (upRssi > -115) blink(2, 40, 120);
        else                    blink(3, 40, 120);
    } else {
        Serial.printf("%u,%d,0,,,,,%.1f,%u\n",
                      ping.seq, txOk ? 1 : 0, pdr, ping.vbat_mv);
        blink(1, 500, 0);
    }
    seq++;
}

void setup() {
    Serial.begin(115200);
    pinMode(USER_LED, OUTPUT);
    digitalWrite(USER_LED, LED_OFF);

    uint32_t t0 = millis();
    while (!Serial && millis() - t0 < 3000) delay(10);

    Serial.println("\n=== LoRa walk test :: camera node (TX) + live camera ===");
    Serial.printf("[cam] PSRAM: %s (%u bytes free)\n",
                  psramFound() ? "yes" : "NO - camera will fail",
                  (unsigned)ESP.getFreePsram());

    // Radio first: if the camera has grabbed PSRAM and something goes wrong we
    // still want the link diagnosed, and a camera fault must not stop the ping.
    radioUp = (loraBegin() == RADIOLIB_ERR_NONE);
    if (!radioUp) {
        Serial.println("[warn] radio init failed - camera will still stream");
    } else {
        pinMode(LORA_DIO1, INPUT);
        uint32_t toa = loraTimeOnAirMs(sizeof(PingPacket));
        uint32_t minInterval = (uint32_t)(toa / LORA_DUTY_CYCLE);
        if (minInterval > pingIntervalMs) pingIntervalMs = minInterval;
        Serial.printf("[duty] airtime %lu ms, ping interval %lu ms\n",
                      (unsigned long)toa, (unsigned long)pingIntervalMs);
    }

    if (!cameraBegin()) {
        Serial.println("[warn] camera init failed - radio will still ping");
    } else {
        Serial.println("[cam] ready, streaming");
    }

    Serial.println("seq,tx_ok,ack,up_rssi,up_snr,dn_rssi,dn_snr,pdr%,vbat_mv");
    lastPing = millis() - pingIntervalMs;  // ping immediately on boot
}

void loop() {
    cameraPollCommands();
    cameraStreamTick();

    if (radioUp && millis() - lastPing >= pingIntervalMs) {
        lastPing = millis();
        sendPing();
    }
    delay(2);
}
