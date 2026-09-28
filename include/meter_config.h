#pragma once
#include <Arduino.h>

// Persistent node settings. Kept in NVS rather than RTC memory so they survive
// a battery change, not just a deep-sleep cycle.
struct MeterConfig {
    // Region of interest, in pixels of the captured frame, around the digit strip.
    uint16_t roi_x, roi_y, roi_w, roi_h;
    uint8_t  digits;        // how many digit cells sit inside the ROI
    uint16_t wake_secs;     // seconds between wakes
    uint8_t  threshold;     // 0 = auto (Otsu), else fixed 0-255
    uint8_t  invert;        // 1 when segments are lighter than the background
    uint16_t lamp_ms;       // illumination settle time before capture
    uint8_t  lamp_bright;   // 0-255 PWM duty for the illumination LEDs
    uint8_t  lamp_hold;     // 1 = keep the lamp lit continuously rather than
                            // only around a capture. Persisted, because at the
                            // meter there is no USB to switch it on from.
                            // Turn it off before any battery deployment.
    uint8_t  heartbeat;     // transmit at least every N cycles, decode or not
    uint8_t  sleep_en;      // 0 = stay awake and transmit on a timer (development)
                            // 1 = deep sleep between wakes (battery deployment)
    uint8_t  flip;          // sensor vertical flip
    uint8_t  mirror;        // sensor horizontal mirror
    uint16_t ap_minutes;    // raise the WiFi viewfinder at boot for this many
                            // minutes; 0 = off. Persisted, because at the meter
                            // there is no USB to enable it from.
    uint16_t rotate;        // 0/90/180/270, applied to the PIXELS before crop
                            // and OCR -- a CSS-only rotation would leave the
                            // decoder slicing digit cells across the digit row
    int16_t  fine_deg;      // additional tilt in TENTHS of a degree, -450..450,
                            // applied after `rotate` about the frame centre.
                            // A bracket on a meter is never square, and the
                            // decoder's vertical cell slices are unforgiving of
                            // even a degree or two.
};

extern MeterConfig CFG;

void meterConfigLoad();
void meterConfigSave();
void meterConfigPrint();
// Handles "SET <key> <value>" lines; returns true if the key was recognised.
bool meterConfigCommand(const String &line);
