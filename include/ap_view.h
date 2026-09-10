#pragma once
#include <Arduino.h>

// Field viewfinder: the node raises its own WiFi access point and serves a live
// view to a phone held next to the meter.
//
// This exists because aiming a camera at a meter in a dark cupboard is the one
// job the LoRa link cannot help with -- at 1% duty cycle the radio moves about
// two bytes a second. Saving frames for later analysis works but forces a walk
// up and down for every adjustment; seeing the picture while your hands are on
// the bracket does not.
//
// Deliberately temporary: it costs ~100 mA and is an open access point, so it
// times out on its own.

void apViewBegin(uint16_t minutes);   // raise the AP; 0 uses the default timeout
void apViewEnd();
bool apViewActive();

// Hand over the newest frame (JPEG, already encoded) plus what was decoded from
// it. Takes ownership of nothing; the buffer is copied.
void apViewPublish(const uint8_t *jpeg, size_t len, const char *status);

// Service HTTP clients. Call often from loop().
void apViewTick();

// Commands typed at the USB console and commands sent from the phone are the
// same commands, so the node registers one handler and both paths use it.
typedef void (*ApCommandHandler)(const String &line);
void apViewSetCommandHandler(ApCommandHandler fn);
