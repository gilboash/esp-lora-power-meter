#pragma once
#include <Arduino.h>

// Seven-segment reader for a fixed-position LCD.
//
// The ISKRA MT171's digit strip never moves relative to the camera once the
// node is mounted, so there is no scene understanding to do: threshold the
// crop, slice it into equal digit cells, and ask which of the seven segments
// are lit. That is far more robust on a low-contrast reflective LCD than any
// small ML model, and costs microseconds.

struct OcrDigit {
    int8_t  value;       // 0-9, or -1 if the segment pattern matches no digit
    uint8_t confidence;  // 0-100: how cleanly segments separated from the mean
    uint8_t segments;    // raw bitmask, bit0=a .. bit6=g, for debugging
    uint8_t fill[7];     // percent of each segment window that read as "on".
                         // The single most useful diagnostic: it says whether a
                         // failed digit is a lighting problem (all values low or
                         // all high) or an alignment problem (plausible values
                         // in the wrong places).
};

struct OcrResult {
    bool     ok;
    uint32_t value;          // digits concatenated, no decimal point applied
    uint8_t  digits;
    uint8_t  confidence;     // the weakest digit's confidence
    uint8_t  threshold_used;
    uint8_t  roi_min, roi_max, roi_mean;   // contrast of the crop, for tuning
    uint16_t trim_x, trim_y, trim_w, trim_h;  // ink bounding box inside the ROI
    bool     by_ink;       // true when digit cells came from measured ink runs
                           // rather than an even division of the ROI
    uint8_t  border_ink;   // percent of the crop's perimeter reading as "on".
                           // High means the ROI has caught the bezel or
                           // surround rather than sitting inside the display --
                           // it defeats the auto-trim and poisons the edge
                           // segment windows, and looks like a decoder fault.
    OcrDigit cell[12];
};

// `gray` is a width*height 8-bit buffer already cropped to the ROI.
OcrResult ocrReadDigits(const uint8_t *gray, int width, int height,
                        uint8_t digits, uint8_t fixedThreshold, bool invert);

// Otsu's method, exposed so the caller can log what a frame chose.
uint8_t ocrOtsuThreshold(const uint8_t *gray, int n);
