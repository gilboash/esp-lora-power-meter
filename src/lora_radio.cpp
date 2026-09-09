#include <Arduino.h>
#include <SPI.h>
#include "board_pins.h"
#include "link_config.h"
#include "lora_radio.h"

// The SX1262 is the only device on this bus during the walk test. On the Sense
// the microSD shares these same three pins, so when SD support is added later
// this SPIClass must be shared rather than re-begun.
static SPIClass loraSpi(FSPI);
static SPISettings loraSpiSettings(2000000, MSBFIRST, SPI_MODE0);

SX1262 radio = new Module(LORA_CS, LORA_DIO1, LORA_RST, LORA_BUSY, loraSpi, loraSpiSettings);

const char *loraErrName(int16_t state) {
    switch (state) {
        case RADIOLIB_ERR_NONE:              return "OK";
        case RADIOLIB_ERR_CHIP_NOT_FOUND:    return "CHIP_NOT_FOUND (check wiring/pins)";
        case RADIOLIB_ERR_SPI_CMD_TIMEOUT:   return "SPI_CMD_TIMEOUT (BUSY pin wrong?)";
        case RADIOLIB_ERR_SPI_CMD_INVALID:   return "SPI_CMD_INVALID";
        case RADIOLIB_ERR_SPI_CMD_FAILED:    return "SPI_CMD_FAILED";
        case RADIOLIB_ERR_INVALID_FREQUENCY: return "INVALID_FREQUENCY";
        case RADIOLIB_ERR_INVALID_OUTPUT_POWER: return "INVALID_OUTPUT_POWER";
        case RADIOLIB_ERR_RX_TIMEOUT:        return "RX_TIMEOUT";
        case RADIOLIB_ERR_CRC_MISMATCH:      return "CRC_MISMATCH";
        default:                             return "see RadioLib status codes";
    }
}

int16_t loraBegin() {
    loraSpi.begin(LORA_SCK, LORA_MISO, LORA_MOSI, LORA_CS);

    int16_t state = radio.begin(LORA_FREQ_MHZ, LORA_BW, LORA_SF, LORA_CR,
                                LORA_SYNC_WORD, LORA_TX_DBM, LORA_PREAMBLE_LEN,
                                LORA_TCXO_VOLTAGE, LORA_USE_LDO);
    if (state != RADIOLIB_ERR_NONE) {
        Serial.printf("[radio] begin failed: %d (%s)\n", state, loraErrName(state));
        return state;
    }

    // The Wio-SX1262 drives the TX side of the RF switch from DIO2 and the RX
    // side from a GPIO. Both must be configured or range collapses.
    state = radio.setDio2AsRfSwitch(true);
    if (state != RADIOLIB_ERR_NONE) {
        Serial.printf("[radio] setDio2AsRfSwitch failed: %d\n", state);
        return state;
    }
    radio.setRfSwitchPins(LORA_RXEN, RADIOLIB_NC);

    // Explicit CRC on the air; the walk test cares about real packet loss, not
    // corrupted frames counted as receptions.
    state = radio.setCRC(2);
    if (state != RADIOLIB_ERR_NONE) {
        Serial.printf("[radio] setCRC failed: %d\n", state);
        return state;
    }

    Serial.printf("[radio] %s up: %.3f MHz, %s, %d dBm\n",
                  NODE_NAME, LORA_FREQ_MHZ, LORA_PROFILE_NAME, LORA_TX_DBM);
    return RADIOLIB_ERR_NONE;
}

uint32_t loraTimeOnAirMs(size_t len) {
    // RadioLib reports time on air in microseconds.
    return (radio.getTimeOnAir(len) + 999) / 1000;
}
