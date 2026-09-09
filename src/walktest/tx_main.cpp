// ---------------------------------------------------------------------------
// Walk-test transmitter — runs on the camera node (XIAO ESP32S3 Sense).
//
// Sends a small ping every interval and waits for the gateway's ACK. Because
// this is the unit you physically carry to the meter, it reports link quality
// on the on-board LED as well as over serial: you can walk the building with
// nothing but a USB power bank and still read the link.
//
//   1 blink   uplink RSSI better than -100 dBm   strong
//   2 blinks  -100 to -115 dBm                   usable
//   3 blinks  worse than -115 dBm                marginal
//   1 long    no ACK                             link down
// ---------------------------------------------------------------------------
#include <Arduino.h>
#include "board_pins.h"
#include "link_config.h"
#include "lora_radio.h"

static volatile bool packetArrived = false;
static uint16_t seq = 0;
static uint32_t sent = 0, acked = 0;
static uint32_t pingIntervalMs = PING_INTERVAL_MS;

ICACHE_RAM_ATTR static void onPacket() { packetArrived = true; }

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
    // Average a few samples; the ADC on the S3 is noisy.
    uint32_t acc = 0;
    for (int i = 0; i < 8; i++) acc += analogReadMilliVolts(VBAT_ADC_PIN);
    return (uint16_t)((acc / 8) * VBAT_DIVIDER);
#else
    return 0;  // no divider fitted — see docs/hardware-notes.md
#endif
}

void setup() {
    Serial.begin(115200);
    pinMode(USER_LED, OUTPUT);
    digitalWrite(USER_LED, LED_OFF);

    uint32_t t0 = millis();
    while (!Serial && millis() - t0 < 3000) delay(10);

    Serial.println("\n=== LoRa walk test :: camera node (TX) ===");

    if (loraBegin() != RADIOLIB_ERR_NONE) {
        Serial.println("[fatal] radio init failed; halting. Check board_pins.h against your module.");
        while (true) { blink(1, 60, 0); delay(400); }
    }

    // Respect the 1% duty-cycle limit: a ping plus the ACK we provoke both
    // occupy the channel, so budget for the pair.
    uint32_t toa = loraTimeOnAirMs(sizeof(PingPacket));
    uint32_t minInterval = (uint32_t)(toa / LORA_DUTY_CYCLE);
    if (minInterval > pingIntervalMs) {
        Serial.printf("[duty] %lu ms airtime needs >= %lu ms spacing at %.0f%% duty; "
                      "raising interval from %lu ms\n",
                      (unsigned long)toa, (unsigned long)minInterval,
                      LORA_DUTY_CYCLE * 100.0f, (unsigned long)pingIntervalMs);
        pingIntervalMs = minInterval;
    }
    Serial.printf("[duty] airtime %lu ms, ping interval %lu ms\n",
                  (unsigned long)toa, (unsigned long)pingIntervalMs);

    radio.setPacketReceivedAction(onPacket);
    Serial.println("seq,tx_ok,ack,up_rssi,up_snr,dn_rssi,dn_snr,pdr%,vbat_mv");
}

void loop() {
    uint32_t cycleStart = millis();

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
        packetArrived = false;
        radio.startReceive();

        uint32_t waitStart = millis();
        while (millis() - waitStart < ACK_TIMEOUT_MS) {
            if (packetArrived) {
                packetArrived = false;
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
                // Not our ACK (stray packet or a stale one): keep listening.
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
        Serial.printf("%u,%d,0,,,,,%.1f,%u%s\n",
                      ping.seq, txOk ? 1 : 0, pdr, ping.vbat_mv,
                      txOk ? "" : "   <- transmit() itself failed");
        blink(1, 500, 0);
    }

    seq++;

    uint32_t elapsed = millis() - cycleStart;
    if (elapsed < pingIntervalMs) delay(pingIntervalMs - elapsed);
}
