#include "meter_ocr.h"

uint8_t ocrOtsuThreshold(const uint8_t *gray, int n) {
    uint32_t hist[256] = {0};
    for (int i = 0; i < n; i++) hist[gray[i]]++;

    uint32_t total = n;
    uint64_t sum = 0;
    for (int i = 0; i < 256; i++) sum += (uint64_t)i * hist[i];

    uint64_t sumB = 0;
    uint32_t wB = 0;
    double best = -1.0;
    uint8_t chosen = 128;
    for (int t = 0; t < 256; t++) {
        wB += hist[t];
        if (wB == 0) continue;
        uint32_t wF = total - wB;
        if (wF == 0) break;
        sumB += (uint64_t)t * hist[t];
        double mB = (double)sumB / wB;
        double mF = (double)(sum - sumB) / wF;
        double between = (double)wB * wF * (mB - mF) * (mB - mF);
        if (between > best) { best = between; chosen = (uint8_t)t; }
    }
    return chosen;
}

// Segment order: a b c d e f g
//        a
//     f     b
//        g
//     e     c
//        d
static const uint8_t SEG_TABLE[10] = {
    0b0111111, // 0: a b c d e f
    0b0000110, // 1: b c
    0b1011011, // 2: a b d e g
    0b1001111, // 3: a b c d g
    0b1100110, // 4: b c f g
    0b1101101, // 5: a c d f g
    0b1111101, // 6: a c d e f g
    0b0000111, // 7: a b c
    0b1111111, // 8: all
    0b1101111, // 9: a b c d f g
};

// Fraction of a segment window that must be "on" for the segment to count as lit.
static const float SEG_ON_FRACTION = 0.45f;

// Sample a rectangle of the binarised cell and return the lit fraction.
static float regionOn(const uint8_t *bin, int w, int h, float fx, float fy, float fw, float fh) {
    int x0 = (int)(fx * w), y0 = (int)(fy * h);
    int x1 = (int)((fx + fw) * w), y1 = (int)((fy + fh) * h);
    x0 = constrain(x0, 0, w - 1); x1 = constrain(x1, 1, w);
    y0 = constrain(y0, 0, h - 1); y1 = constrain(y1, 1, h);
    if (x1 <= x0 || y1 <= y0) return 0.0f;
    uint32_t on = 0, n = 0;
    for (int y = y0; y < y1; y++) {
        for (int x = x0; x < x1; x++) { on += bin[y * w + x] ? 1 : 0; n++; }
    }
    return n ? (float)on / n : 0.0f;
}

OcrResult ocrReadDigits(const uint8_t *gray, int width, int height,
                        uint8_t digits, uint8_t fixedThreshold, bool invert) {
    OcrResult r = {};
    r.digits = digits;
    if (!gray || width < 8 || height < 8 || digits == 0 || digits > 12) return r;

    uint8_t thr = fixedThreshold ? fixedThreshold
                                 : ocrOtsuThreshold(gray, width * height);
    r.threshold_used = thr;

    // Contrast of the crop. A narrow min..max range means the picture, not the
    // decoder, is the problem -- no threshold separates what is not separated.
    uint8_t lo = 255, hi = 0;
    uint32_t sum = 0;
    const int n = width * height;
    for (int i = 0; i < n; i++) {
        uint8_t v = gray[i];
        if (v < lo) lo = v;
        if (v > hi) hi = v;
        sum += v;
    }
    r.roi_min = lo; r.roi_max = hi; r.roi_mean = (uint8_t)(sum / n);

    // How much of the crop's perimeter reads as lit.
    uint32_t edgeOn = 0, edgeN = 0;
    for (int x = 0; x < width; x++) {
        for (int y : {0, height - 1}) {
            uint8_t v = gray[(size_t)y * width + x];
            if (invert ? (v > thr) : (v < thr)) edgeOn++;
            edgeN++;
        }
    }
    for (int y = 1; y < height - 1; y++) {
        for (int x : {0, width - 1}) {
            uint8_t v = gray[(size_t)y * width + x];
            if (invert ? (v > thr) : (v < thr)) edgeOn++;
            edgeN++;
        }
    }
    r.border_ink = edgeN ? (uint8_t)(100 * edgeOn / edgeN) : 0;

    // Shrink to the ink before slicing cells.
    //
    // Segment windows are placed as fractions of a digit cell, so a ROI drawn
    // even slightly too tall pushes every window off the digits -- which looks
    // exactly like a lighting failure but is not. Rather than demand a
    // pixel-perfect ROI, find the bounding box of the ink by row/column
    // projection and work inside that. Drawing the ROI becomes forgiving.
    static uint16_t colInk[640], rowInk[480];
    int bw = min(width, 640), bh = min(height, 480);
    for (int i = 0; i < bw; i++) colInk[i] = 0;
    for (int i = 0; i < bh; i++) rowInk[i] = 0;
    for (int y = 0; y < bh; y++) {
        const uint8_t *row = gray + (size_t)y * width;
        for (int x = 0; x < bw; x++) {
            bool on = invert ? (row[x] > thr) : (row[x] < thr);
            if (on) { colInk[x]++; rowInk[y]++; }
        }
    }
    // A line counts as containing ink only above a floor, so specks and bezel
    // glints do not widen the box.
    //
    // This floor was 2% and that was far too generous: three stray bright
    // pixels near the top of the crop were enough to hold the box open, leaving
    // every segment window shifted up so the top segments sampled empty space.
    // A row that genuinely crosses the digits carries ink across a large
    // fraction of the width, so 6% is still very safe, and a column through the
    // thinnest part of a digit still clears 6% of the height.
    int rowFloor = max(2, bw / 16);
    int colFloor = max(2, bh / 16);
    int x0 = 0, x1 = bw - 1, y0 = 0, y1 = bh - 1;
    while (x0 < x1 && colInk[x0] < colFloor) x0++;
    while (x1 > x0 && colInk[x1] < colFloor) x1--;
    while (y0 < y1 && rowInk[y0] < rowFloor) y0++;
    while (y1 > y0 && rowInk[y1] < rowFloor) y1--;

    int tw = x1 - x0 + 1, th = y1 - y0 + 1;
    // Refuse a degenerate trim (a blank or saturated crop) and fall back to the
    // ROI as drawn, so a bad frame cannot wedge the decoder.
    if (tw < digits * 4 || th < 8) { x0 = 0; y0 = 0; tw = width; th = height; }
    r.trim_x = x0; r.trim_y = y0; r.trim_w = tw; r.trim_h = th;

    const uint8_t *base = gray + (size_t)y0 * width + x0;
    if (tw / digits < 4) return r;

    // Locate each digit by its own ink columns rather than dividing the box
    // evenly.
    //
    // Seven-segment digits are narrower than their pitch: measured on a real
    // display, three digits of width 37/39/47 sat inside 50-pixel slices, all
    // flush left. Equal division therefore puts the b and c windows (72-96% of
    // cell width) in the gap *after* each digit, so those segments read ~10
    // when they should read ~100 -- indistinguishable from a lighting fault.
    struct Run { int a, b; };
    Run runs[24];
    int nRuns = 0;
    int start = -1;
    int colFloorDigit = max(1, (int)(th * 0.05f));
    for (int x = 0; x <= tw; x++) {
        int count = 0;
        if (x < tw) {
            for (int y = 0; y < th; y++) {
                uint8_t v = base[(size_t)y * width + x];
                if (invert ? (v > thr) : (v < thr)) count++;
            }
        }
        bool on = (x < tw) && (count > colFloorDigit);
        if (on && start < 0) start = x;
        if (!on && start >= 0) {
            if (nRuns < (int)(sizeof(runs) / sizeof(runs[0]))) {
                runs[nRuns++] = {start, x - 1};
            }
            start = -1;
        }
    }
    // Drop decimal points and specks before matching the run count.
    if (nRuns > 0) {
        int widest = 0;
        for (int i = 0; i < nRuns; i++) widest = max(widest, runs[i].b - runs[i].a + 1);
        int keep = 0;
        for (int i = 0; i < nRuns; i++) {
            if (runs[i].b - runs[i].a + 1 >= max(3, (int)(widest * 0.25f)))
                runs[keep++] = runs[i];
        }
        nRuns = keep;
    }

    int cellA[12], cellB[12];
    bool byInk = (nRuns == digits);
    if (byInk) {
        int widths[12];
        for (int i = 0; i < digits; i++) widths[i] = runs[i].b - runs[i].a + 1;
        for (int i = 1; i < digits; i++) {          // insertion sort for median
            int v = widths[i], j = i - 1;
            while (j >= 0 && widths[j] > v) { widths[j + 1] = widths[j]; j--; }
            widths[j + 1] = v;
        }
        int median = widths[digits / 2];
        for (int i = 0; i < digits; i++) {
            int a = runs[i].a, b = runs[i].b;
            // A "1" is only segments b and c, so its run is a thin bar. Left as
            // a whole cell it would look like every segment was lit, so widen it
            // to the median and anchor right, where b and c actually sit.
            if ((b - a + 1) < median / 2) a = max(0, b - median + 1);
            cellA[i] = a; cellB[i] = b;
        }
    } else {
        int cw = tw / digits;
        for (int i = 0; i < digits; i++) { cellA[i] = i * cw; cellB[i] = (i + 1) * cw - 1; }
    }
    r.by_ink = byInk;

    // Segment windows as fractions of one digit cell. Deliberately inset so a
    // slight mounting rotation does not push a window onto its neighbour.
    struct { float x, y, w, h; } SEG[7] = {
        {0.25f, 0.02f, 0.50f, 0.16f},  // a  top
        {0.72f, 0.12f, 0.24f, 0.32f},  // b  top right
        {0.72f, 0.55f, 0.24f, 0.32f},  // c  bottom right
        {0.25f, 0.82f, 0.50f, 0.16f},  // d  bottom
        {0.04f, 0.55f, 0.24f, 0.32f},  // e  bottom left
        {0.04f, 0.12f, 0.24f, 0.32f},  // f  top left
        {0.25f, 0.42f, 0.50f, 0.16f},  // g  middle
    };

    uint32_t value = 0;
    uint8_t worst = 100;
    bool allOk = true;

    // Binarise once into a scratch row-major buffer per cell to keep it simple.
    for (uint8_t d = 0; d < digits; d++) {
        int cx = cellA[d];
        int cellW = cellB[d] - cellA[d] + 1;
        if (cellW < 4) { r.ok = false; continue; }
        static uint8_t bin[64 * 128];
        int cw = min(cellW, 64), ch = min(th, 128);
        for (int y = 0; y < ch; y++) {
            for (int x = 0; x < cw; x++) {
                int sx = cx + (x * cellW) / cw;
                int sy = (y * th) / ch;
                uint8_t v = base[(size_t)sy * width + sx];
                bool on = invert ? (v > thr) : (v < thr);
                bin[y * cw + x] = on ? 1 : 0;
            }
        }

        uint8_t mask = 0;
        float margin = 1.0f;
        for (int s = 0; s < 7; s++) {
            float f = regionOn(bin, cw, ch, SEG[s].x, SEG[s].y, SEG[s].w, SEG[s].h);
            r.cell[d].fill[s] = (uint8_t)(f * 100.0f + 0.5f);
            if (f >= SEG_ON_FRACTION) mask |= (1 << s);
            // How far this segment sat from the decision line; the closest call
            // across all seven is what the digit's confidence reflects.
            margin = min(margin, fabsf(f - SEG_ON_FRACTION));
        }

        OcrDigit &cell = r.cell[d];
        cell.segments = mask;
        cell.value = -1;
        for (int n = 0; n < 10; n++) {
            if (SEG_TABLE[n] == mask) { cell.value = n; break; }
        }
        cell.confidence = (uint8_t)constrain((int)(margin / SEG_ON_FRACTION * 100.0f), 0, 100);

        if (cell.value < 0) { allOk = false; cell.confidence = 0; }
        else value = value * 10 + cell.value;
        worst = min(worst, cell.confidence);
    }

    r.ok = allOk;
    r.value = value;
    r.confidence = worst;
    return r;
}
