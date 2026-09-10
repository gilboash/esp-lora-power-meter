// Host-side regression test for the seven-segment decoder.
//
// Replays a crop captured from real hardware (a soldering station showing 290)
// through the very same C++ the node runs, so a change to the segment geometry
// cannot quietly break decoding without a board and a lit display to notice on.
#include <cstdio>
#include <cstdlib>
#include <vector>
#include "meter_ocr.h"

struct Case { const char *path; int w, h, digits; bool invert; const char *expect; };

int main() {
    const Case cases[] = {
        {"tests/fixtures/iron_290_roi.raw", 158, 75, 3, true, "290"},
    };
    int failures = 0;
    for (const Case &c : cases) {
        FILE *f = fopen(c.path, "rb");
        if (!f) { printf("FAIL  cannot open %s\n", c.path); failures++; continue; }
        std::vector<uint8_t> buf((size_t)c.w * c.h);
        size_t got = fread(buf.data(), 1, buf.size(), f);
        fclose(f);
        if (got != buf.size()) { printf("FAIL  short read %s\n", c.path); failures++; continue; }

        OcrResult r = ocrReadDigits(buf.data(), c.w, c.h, c.digits, 0, c.invert);
        char text[13] = {0};
        for (int i = 0; i < c.digits; i++)
            text[i] = r.cell[i].value >= 0 ? char('0' + r.cell[i].value) : '?';

        bool pass = (strcmp(text, c.expect) == 0);
        printf("%s  %s -> '%s' (expected '%s')  thr=%u edge=%u%% trim=%ux%u by_ink=%d\n",
               pass ? "PASS " : "FAIL ", c.path, text, c.expect,
               r.threshold_used, r.border_ink, r.trim_w, r.trim_h, (int)r.by_ink);
        for (int i = 0; i < c.digits; i++) {
            printf("        d%d:", i);
            const char *n = "abcdefg";
            for (int k = 0; k < 7; k++) printf(" %c=%3u", n[k], r.cell[i].fill[k]);
            printf("\n");
        }
        if (!pass) failures++;
    }
    printf("\n%s\n", failures ? "TESTS FAILED" : "all tests passed");
    return failures ? 1 : 0;
}
