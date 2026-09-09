#pragma once
#include <RadioLib.h>

// Shared SX1262 instance and bring-up, identical for both nodes apart from the
// pin map pulled in from board_pins.h.
extern SX1262 radio;

// Brings up SPI and the radio with the profile from link_config.h.
// Returns RADIOLIB_ERR_NONE on success; prints a decoded failure otherwise.
int16_t loraBegin();

// Human-readable RadioLib status, for log lines.
const char *loraErrName(int16_t state);

// Time on air, in milliseconds, for a payload of `len` bytes at the active profile.
uint32_t loraTimeOnAirMs(size_t len);
