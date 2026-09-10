// ---------------------------------------------------------------------------
// Battery meter node: wake on a timer, photograph the meter's digit strip,
// decode it, send the number over LoRa, sleep.
//
// Two modes, chosen automatically at boot:
//
//   USB present  -> BENCH mode. Stays awake, streams frames so the ROI can be
//                   drawn on the dashboard, re-runs the decode every second and
//                   reports what it read. This is how the ROI gets calibrated.
//   No USB       -> FIELD mode. One measurement, one transmission, then deep
//                   sleep for CFG.wake_secs.
//
// Power reality on this hardware (measured by others, see docs/hardware-notes):
// the Sense board keeps its regulators live in deep sleep, so ~2 mA is the
// floor even with the camera deinitialised, and ~90 mA if it is not. Camera
// capture, not the radio, dominates each wake -- roughly 250 mA*s against
// 60 mA*s for an SF12 transmit. Wake interval is therefore the main lever on
// battery life, and change-detection gating saves less than the brief assumed.
// ---------------------------------------------------------------------------
#include <Arduino.h>
#include <esp_sleep.h>
#include "board_pins.h"
#include "link_config.h"
#include "lora_radio.h"
#include "camera_hw.h"
#include "meter_config.h"
#include "meter_ocr.h"
#include "ap_view.h"

// Survives deep sleep (but not a power cut -- config lives in NVS instead).
RTC_DATA_ATTR static uint32_t bootCount = 0;
RTC_DATA_ATTR static uint32_t lastValue = 0;
RTC_DATA_ATTR static uint32_t lastSentValue = 0xFFFFFFFF;
RTC_DATA_ATTR static uint32_t wakesSinceSend = 0;
RTC_DATA_ATTR static uint16_t txSeq = 0;

// Send at least this often even when the reading has not moved, so a silent
// node is distinguishable from a node whose meter simply is not ticking.
static const int MEASURE_FRAMESIZE = 10;  // FRAMESIZE_VGA, 640x480

// Camera preview is a debugging tool, opted into at runtime and never
// persisted: attaching USB must not change what the node does, or the thing you
// debug is not the thing that runs in the field. Off after every reboot.
static bool previewEnabled = false;
// PREVIEW 2 sends the binarised ROI instead of the frame, which is the only
// way to see why a decode failed without guessing.
static bool previewBinary = false;
static bool radioUp = false;

static void pollCommandLines();
static void handleCommandLine(const String &line);

// ACK_TIMEOUT_MS is a sane floor at SF7-SF9, but an 8-byte ACK at SF12 needs
// ~990 ms of airtime on its own -- longer than the 800 ms constant. Waiting too
// little makes every successful transmission look failed: the node retries,
// the gateway logs the same reading two or three times, and the LED reports
// "no ack" while the link is in fact perfect. Derived from the active profile.
static uint32_t ackTimeoutMs = ACK_TIMEOUT_MS;

// Visible proof of life while running on battery, with no USB attached:
// one blink at boot, two on an acknowledged transmit, one long on no ack.
static void blinkLed(uint8_t times, uint16_t onMs, uint16_t offMs) {
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

static inline bool packetReady() {
    if (digitalRead(LORA_DIO1) != HIGH) return false;
    return (radio.getIrqFlags() & (1UL << RADIOLIB_IRQ_RX_DONE)) != 0;
}

// Rotate a grayscale frame by 0/90/180/270 into a PSRAM scratch buffer.
//
// This has to happen to the pixels, not just in the browser: the decoder slices
// each ROI into equal vertical columns, so if the node is mounted sideways those
// columns cut across the digits instead of separating them, and no amount of
// threshold tuning can recover it. Rotating here means the preview, the ROI you
// draw on it, and the decode all share one coordinate system.
static uint8_t *rotateGray(const uint8_t *src, int sw, int sh, int deg,
                           int *dw, int *dh) {
    if (deg == 0) { *dw = sw; *dh = sh; return (uint8_t *)src; }

    static uint8_t *buf = nullptr;
    if (!buf) buf = (uint8_t *)ps_malloc(640 * 480);
    if (!buf) { *dw = sw; *dh = sh; return (uint8_t *)src; }

    if (deg == 180) {
        *dw = sw; *dh = sh;
        const size_t n = (size_t)sw * sh;
        for (size_t i = 0; i < n; i++) buf[i] = src[n - 1 - i];
        return buf;
    }
    // 90 and 270 swap the axes.
    *dw = sh; *dh = sw;
    for (int y = 0; y < sh; y++) {
        for (int x = 0; x < sw; x++) {
            uint8_t v = src[(size_t)y * sw + x];
            if (deg == 90) buf[(size_t)x * sh + (sh - 1 - y)] = v;   // clockwise
            else           buf[(size_t)(sw - 1 - x) * sh + y] = v;   // 270
        }
    }
    return buf;
}

// Burn the ROI outline into the preview frame itself.
//
// The alternative is to draw the box in the browser and trust that the page's
// coordinate mapping matches the firmware's. Drawing it here removes that trust:
// whatever rectangle you see in the preview is, by construction, the exact
// rectangle the decoder crops. Drawn after the decode so it cannot pollute it.
static inline void px(uint8_t *img, int w, int h, int x, int y, uint8_t v) {
    if (x >= 0 && y >= 0 && x < w && y < h) img[(size_t)y * w + x] = v;
}

static void drawRoiOverlay(uint8_t *img, int w, int h) {
    int rx = CFG.roi_w ? CFG.roi_x : 0;
    int ry = CFG.roi_w ? CFG.roi_y : 0;
    int rw = CFG.roi_w ? CFG.roi_w : w;
    int rh = CFG.roi_h ? CFG.roi_h : h;
    if (rx >= w || ry >= h) return;
    rw = min(rw, w - rx);
    rh = min(rh, h - ry);
    if (rw <= 1 || rh <= 1) return;

    // Two-tone edge so it stays visible over both dark and light content.
    for (int i = 0; i < rw; i++) {
        px(img, w, h, rx + i, ry,          255); px(img, w, h, rx + i, ry + 1,      0);
        px(img, w, h, rx + i, ry + rh - 1, 255); px(img, w, h, rx + i, ry + rh - 2, 0);
    }
    for (int i = 0; i < rh; i++) {
        px(img, w, h, rx,          ry + i, 255); px(img, w, h, rx + 1,      ry + i, 0);
        px(img, w, h, rx + rw - 1, ry + i, 255); px(img, w, h, rx + rw - 2, ry + i, 0);
    }
    // Cell boundaries, dashed: these are where the decoder splits digits, so a
    // divider sitting on a digit rather than between two is the bug to spot.
    for (int d = 1; d < CFG.digits; d++) {
        int cx = rx + (rw * d) / CFG.digits;
        for (int i = 0; i < rh; i += 6) {
            px(img, w, h, cx,     ry + i,     255);
            px(img, w, h, cx + 1, ry + i,     0);
            px(img, w, h, cx,     ry + i + 1, 255);
            px(img, w, h, cx + 1, ry + i + 1, 0);
        }
    }
}

// Crop the ROI out of a full grayscale frame into `dst`.
// A zero-width ROI means "the whole frame", so an unconfigured node still
// produces a usable preview rather than nothing at all.
static bool cropRoi(const uint8_t *src, int sw, int sh,
                    uint8_t *dst, int *dw, int *dh) {
    int x = CFG.roi_w ? CFG.roi_x : 0;
    int y = CFG.roi_w ? CFG.roi_y : 0;
    int w = CFG.roi_w ? CFG.roi_w : sw;
    int h = CFG.roi_h ? CFG.roi_h : sh;
    if (x >= sw || y >= sh) return false;
    w = min(w, sw - x);
    h = min(h, sh - y);
    if (w <= 0 || h <= 0) return false;
    for (int r = 0; r < h; r++) {
        memcpy(dst + (size_t)r * w, src + (size_t)(y + r) * sw + x, w);
    }
    *dw = w; *dh = h;
    return true;
}

// One capture + decode. Returns false if the camera could not produce a frame.
// `keepOpen` decides who owns the sensor's lifecycle.
//
// The transmit cycle wants it closed straight afterwards -- leaving the camera
// initialised is the difference between ~2 mA and ~90 mA asleep. But tearing it
// down between *preview* frames is ruinous: a full init plus two captures takes
// well over a second, so frames arrive slower than the dashboard's liveness
// window and the video visibly flickers as the stream tears down and reconnects.
// While previewing we therefore init once and hold it.
static bool measure(OcrResult *out, bool sendPreview, bool keepOpen,
                    bool usbPreview = true) {
    digitalWrite(LAMP_PIN, LAMP_ON);
    delay(CFG.lamp_ms);                  // let the LED and AGC settle

    bool wasOpen = cameraIsOpen();
    if (!wasOpen && !cameraBeginGray(MEASURE_FRAMESIZE)) {
        digitalWrite(LAMP_PIN, LAMP_OFF);
        return false;
    }
    camera_fb_t *fb = nullptr;
    if (!wasOpen) {
        // The first frame after init is exposed with the sensor's power-on gain;
        // throw it away so the decode sees a properly metered image. Only needed
        // on a fresh init -- an already-running sensor is settled.
        fb = esp_camera_fb_get();
        if (fb) { esp_camera_fb_return(fb); fb = nullptr; }
    }
    fb = esp_camera_fb_get();

    bool ok = false;
    if (fb && fb->format == PIXFORMAT_GRAYSCALE) {
        static uint8_t *roi = nullptr;
        if (!roi) roi = (uint8_t *)ps_malloc(640 * 480);
        int fw = 0, fh = 0;
        uint8_t *frame = rotateGray(fb->buf, fb->width, fb->height, CFG.rotate, &fw, &fh);
        int rw = 0, rh = 0;
        if (roi && cropRoi(frame, fw, fh, roi, &rw, &rh)) {
            *out = ocrReadDigits(roi, rw, rh, CFG.digits, CFG.threshold, CFG.invert);
            ok = true;
            if (sendPreview) {
                if (previewBinary) {
                    // Show exactly what the thresholder sees. If the digits are
                    // not solid black on solid white here, the problem is the
                    // threshold or the lighting, not the decoder.
                    uint8_t thr = CFG.threshold ? CFG.threshold
                                                : ocrOtsuThreshold(roi, rw * rh);
                    for (int i = 0; i < rw * rh; i++) {
                        bool on = CFG.invert ? (roi[i] > thr) : (roi[i] < thr);
                        roi[i] = on ? 0 : 255;
                    }
                    cameraSendGrayPreview(roi, rw, rh, 16);
                } else {
                    drawRoiOverlay(frame, fw, fh);
                    if (apViewActive()) {
                        // Same overlaid frame the USB preview shows, so the ROI
                        // you see on the phone is the one being decoded.
                        uint8_t *jpg = nullptr;
                        size_t jlen = 0;
                        if (cameraEncodeGray(frame, fw, fh, 14, &jpg, &jlen)) {
                            char st[96];
                            snprintf(st, sizeof(st),
                                     "%s  conf %u  thr %u  edge %u%%",
                                     out->ok ? "reading" : "no decode",
                                     out->confidence, out->threshold_used,
                                     out->border_ink);
                            // Overwrite the placeholder with the decoded digits.
                            char digits[16] = {0};
                            for (uint8_t i = 0; i < out->digits && i < 12; i++)
                                digits[i] = out->cell[i].value >= 0
                                          ? char('0' + out->cell[i].value) : '?';
                            char full[96];
                            snprintf(full, sizeof(full), "%s\n%s", digits, st);
                            apViewPublish(jpg, jlen, full);
                            free(jpg);
                        }
                    }
                    if (usbPreview) cameraSendGrayPreview(frame, fw, fh, 12);
                }
            }
        }
    }
    if (fb) esp_camera_fb_return(fb);

    if (!keepOpen) cameraEnd();
    digitalWrite(LAMP_PIN, LAMP_OFF);
    return ok;
}

static bool sendReading(const OcrResult &r) {
    ReadingPacket pkt = {};
    pkt.magic = MSG_MAGIC_READ;
    pkt.version = MSG_VERSION;
    pkt.seq = ++txSeq;
    pkt.value = r.value;
    pkt.vbat_mv = readBatteryMv();
    pkt.digits = r.digits;
    pkt.confidence = r.confidence;
    pkt.flags = r.ok ? READING_FLAG_DECODE_OK : 0;

    // Up to three attempts: a single lost packet underground should not cost a
    // whole wake interval of data.
    for (int attempt = 0; attempt < 3; attempt++) {
        if (radio.transmit((uint8_t *)&pkt, sizeof(pkt)) != RADIOLIB_ERR_NONE) continue;
        radio.clearIrqFlags(0xFFFFFFFFUL);
        radio.startReceive();
        uint32_t t0 = millis();
        while (millis() - t0 < ackTimeoutMs) {
            if (packetReady()) {
                AckPacket ack = {};
                if (radio.readData((uint8_t *)&ack, sizeof(ack)) == RADIOLIB_ERR_NONE &&
                    ack.magic == MSG_MAGIC_ACK && ack.seq == pkt.seq) {
                    radio.standby();
                    return true;
                }
                radio.startReceive();
            }
            delay(1);
        }
        radio.standby();
        delay(200);
    }
    return false;
}

static void reportOcr(const OcrResult &r) {
    Serial.printf("[ocr] value=%lu ok=%d conf=%u thr=%u segs=",
                  (unsigned long)r.value, r.ok, r.confidence, r.threshold_used);
    for (uint8_t i = 0; i < r.digits; i++) {
        if (r.cell[i].value >= 0) Serial.printf("%d", r.cell[i].value);
        else                      Serial.print('?');
    }
    Serial.println();

    // Segment detail only while previewing: it is for tuning at the bench, and
    // would be noise in the field log.
    if (previewEnabled) {
        Serial.printf("[roi] min=%u mean=%u max=%u contrast=%u thr=%u trim=%u,%u %ux%u edge=%u\n",
                      r.roi_min, r.roi_mean, r.roi_max,
                      (unsigned)(r.roi_max - r.roi_min), r.threshold_used,
                      r.trim_x, r.trim_y, r.trim_w, r.trim_h, r.border_ink);
        static const char *SEGN = "abcdefg";
        for (uint8_t i = 0; i < r.digits; i++) {
            Serial.printf("[seg] d%u:", i);
            for (int k = 0; k < 7; k++)
                Serial.printf(" %c=%u", SEGN[k], r.cell[i].fill[k]);
            Serial.printf("  -> %c\n", r.cell[i].value >= 0 ? ('0' + r.cell[i].value) : '?');
        }
    }
}

static void goToSleep() {
    Serial.printf("[sleep] %u s\n", CFG.wake_secs);
    Serial.flush();
    esp_sleep_enable_timer_wakeup((uint64_t)CFG.wake_secs * 1000000ULL);
    esp_deep_sleep_start();
}

void setup() {
    Serial.begin(115200);
    pinMode(LAMP_PIN, OUTPUT);
    digitalWrite(LAMP_PIN, LAMP_OFF);
    pinMode(USER_LED, OUTPUT);
    digitalWrite(USER_LED, LED_OFF);

    bootCount++;
    meterConfigLoad();
    blinkLed(1, 60, 0);   // "I woke up", visible on battery

    // Deciding bench vs field. How long to wait for USB depends on why we
    // booted, because the two cases want opposite things:
    //
    //   timer wake  -> we are on battery in a meter cupboard. USB is very
    //                  unlikely, and every millisecond awake costs charge, so
    //                  glance and move on.
    //   any other   -> power-on, reset button, or a USB plug event. Someone is
    //                  probably at the bench, and USB enumeration after a cold
    //                  boot can take a couple of seconds. Waiting too little
    //                  here strands the node: it sleeps again before the host
    //                  enumerates, and bench mode becomes unreachable without
    //                  pulling power.
    // Wait briefly for USB purely so early logging is visible; a timer wake on
    // battery gets a much shorter window because every millisecond costs charge.
    esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
    uint32_t usbWaitMs = (cause == ESP_SLEEP_WAKEUP_TIMER) ? 400 : 2000;
    uint32_t t0 = millis();
    while (!Serial && millis() - t0 < usbWaitMs) delay(10);

    Serial.printf("\n=== meter node (boot #%lu) ===\n", (unsigned long)bootCount);
    Serial.printf("[cam] PSRAM %s, %u bytes free\n",
                  psramFound() ? "ok" : "MISSING", (unsigned)ESP.getFreePsram());
    meterConfigPrint();
    Serial.println("Commands: SET <key> <n> | PREVIEW 0|1|2 | SEND | AP <minutes>|0");
    apViewSetCommandHandler(handleCommandLine);

    // Bring the viewfinder up on its own when configured. Without this the AP
    // is unreachable in the field: enabling it needs a command, and at the
    // meter there is no USB to send one from.
    if (CFG.ap_minutes) {
        Serial.printf("[ap] auto-start for %u min (SET ap 0 to disable)\n",
                      CFG.ap_minutes);
        apViewBegin(CFG.ap_minutes);
    }
    // Restore orientation before the first capture, so the very first frame and
    // every ROI drawn against it share one coordinate system.
    cameraSetOrientation(CFG.flip, CFG.mirror);

    radioUp = (loraBegin() == RADIOLIB_ERR_NONE);
    if (!radioUp) {
        Serial.println("[warn] radio init failed; will retry while running");
    } else {
        pinMode(LORA_DIO1, INPUT);
        ackTimeoutMs = loraTimeOnAirMs(sizeof(AckPacket)) + ACK_TURNAROUND_MS + 400;
        if (ackTimeoutMs < ACK_TIMEOUT_MS) ackTimeoutMs = ACK_TIMEOUT_MS;
        Serial.printf("[radio] ack window %lu ms\n", (unsigned long)ackTimeoutMs);
        // Not enforced -- a short interval is legitimate on the bench -- but it
        // must never be invisible. At SF12 a reading is ~1.2 s of airtime, so a
        // 5 s wake sits around 23% duty against the 1% limit for EU 868.
        uint32_t toa = loraTimeOnAirMs(sizeof(ReadingPacket));
        uint32_t minGap = (uint32_t)(toa / LORA_DUTY_CYCLE) / 1000;
        if (CFG.wake_secs < minGap) {
            Serial.printf("[duty] WARNING wake=%us but %lu ms airtime needs >=%lus "
                          "for %.0f%% duty (currently ~%.1f%%). Bench only.\n",
                          CFG.wake_secs, (unsigned long)toa, (unsigned long)minGap,
                          LORA_DUTY_CYCLE * 100.0f,
                          100.0f * toa / (CFG.wake_secs * 1000.0f));
        }
    }

    // Deep sleep is opt-in. During development the node stays awake and
    // transmits on a timer, so the dashboard shows a continuously live link
    // instead of a node that is healthy but silent between wakes.
    if (!CFG.sleep_en) {
        Serial.printf("[mode] CONTINUOUS - no deep sleep, transmit every %us\n",
                      CFG.wake_secs);
        return;   // loop() drives everything
    }
    // Sleep is enabled. Offer a short command window on anything other than a
    // timer wake, so a node configured to sleep is still reachable to
    // reconfigure -- otherwise enabling sleep locks you out of it.
    if (cause != ESP_SLEEP_WAKEUP_TIMER) {
        Serial.println("[mode] 3 s command window, then sleeping");
        uint32_t w0 = millis();
        while (millis() - w0 < 3000) { pollCommandLines(); delay(10); }
        if (!CFG.sleep_en) {
            Serial.println("[mode] sleep disabled during window - staying awake");
            return;
        }
    }

    // ---- field mode: one shot, then sleep ----
    OcrResult r = {};
    if (!measure(&r, false, false)) {
        Serial.println("[err] capture failed");
        goToSleep();
    }
    reportOcr(r);

    // Transmit whenever the reading moved, on a periodic heartbeat, and always
    // on a cold boot. The cold-boot case is what makes fitting the battery at
    // the meter self-verifying: power it up and the gateway hears it at once.
    // Crucially the heartbeat is NOT gated on a successful decode -- a node that
    // cannot read the display must still prove it is alive, or a failed decode
    // is indistinguishable from a flat battery.
    bool coldBoot = (txSeq == 0);
    bool changed = r.ok && (r.value != lastSentValue);
    bool heartbeat = (++wakesSinceSend >= CFG.heartbeat);
    if (changed || heartbeat || coldBoot) {
        if (sendReading(r)) {
            if (r.ok) lastSentValue = r.value;
            wakesSinceSend = 0;
            Serial.println("[tx] acked");
            blinkLed(2, 40, 120);
        } else {
            Serial.println("[tx] no ack");
            blinkLed(1, 500, 0);
        }
    } else {
        Serial.println("[tx] skipped (unchanged)");
    }
    lastValue = r.value;
    goToSleep();
}

static OcrResult lastResult = {};


// Shared by both modes: transmit when the value moved, on the heartbeat, or on
// the very first cycle. Never gated on a successful decode -- a node that
// cannot read the display must still prove it is alive.
static void transmitIfDue(const OcrResult &r) {
    bool coldStart = (txSeq == 0);
    bool changed = r.ok && (r.value != lastSentValue);
    bool heartbeat = (++wakesSinceSend >= CFG.heartbeat);
    if (!(changed || heartbeat || coldStart)) {
        Serial.println("[tx] skipped (unchanged)");
        return;
    }
    if (sendReading(r)) {
        if (r.ok) lastSentValue = r.value;
        wakesSinceSend = 0;
        Serial.println("[tx] acked");
        blinkLed(2, 40, 120);
    } else {
        Serial.println("[tx] no ack");
        blinkLed(1, 500, 0);
    }
}

// One implementation, two callers: the USB console and the phone over the AP.
// Splitting them would let the two drift, and the phone is the one that will be
// used at the meter where mistakes are expensive to discover.
static void handleCommandLine(const String &line) {
    {
        {
            {
                if (line == "SEND") {
                    OcrResult r = {};
                    if (measure(&r, previewEnabled, previewEnabled)) { reportOcr(r); transmitIfDue(r); }
                    else Serial.println("[err] capture failed");
                } else if (line.startsWith("FLIP ") || line.startsWith("MIRROR ")) {
                    bool on = line.substring(line.indexOf(' ') + 1).toInt() != 0;
                    if (line.startsWith("FLIP ")) CFG.flip = on; else CFG.mirror = on;
                    meterConfigSave();
                    cameraSetOrientation(CFG.flip, CFG.mirror);
                    meterConfigPrint();
                } else if (line.startsWith("AP ")) {
                    int mins = line.substring(3).toInt();
                    if (mins > 0) apViewBegin((uint16_t)mins);
                    else          apViewEnd();
                } else if (line.startsWith("PREVIEW ")) {
                    int mode = line.substring(8).toInt();
                    previewEnabled = mode != 0;
                    previewBinary = (mode == 2);
                    if (!previewEnabled) cameraEnd();   // stop holding the sensor
                    Serial.printf("[preview] %s\n", previewEnabled ? (previewBinary ? "binary" : "on") : "off");
                } else if (!meterConfigCommand(line)) {
                    cameraPollCommandLine(line);
                }
            }
        }
    }
}

static void pollCommandLines() {
    static String line;
    while (Serial.available()) {
        char ch = (char)Serial.read();
        if (ch == '\n' || ch == '\r') {
            if (line.length()) { handleCommandLine(line); line = ""; }
        } else if (line.length() < 64) {
            line += ch;
        }
    }
}

void loop() {
    pollCommandLines();

    apViewTick();

    const bool usb = (bool)Serial;
    const uint32_t now = millis();

    // Re-announce config: the dashboard is usually started long after the node
    // booted and must reflect what the hardware actually holds.
    static uint32_t lastCfg = 0;
    if (usb && now - lastCfg > 5000) { lastCfg = now; meterConfigPrint(); }

    // Capture whenever a viewer wants frames -- the USB preview when explicitly
    // enabled, or the WiFi viewfinder whenever it is up. The AP case must not be
    // gated on USB: at the meter there is no USB, which is the entire point of
    // having it. With neither viewer active the node behaves exactly as it will
    // in the field.
    static uint32_t lastPreview = 0;
    bool usbPreviewing = previewEnabled && usb;
    bool previewing = usbPreviewing || apViewActive();
    if (previewing && now - lastPreview >= 200) {   // ~5 fps, sensor stays open
        lastPreview = now;
        if (measure(&lastResult, true, true, usbPreviewing)) {
            if (usbPreviewing) reportOcr(lastResult);
        } else if (usbPreviewing) {
            Serial.println("[err] capture failed");
        }
    }

    // Same reasoning as the gateway: a node that gives up on its radio for good
    // is indistinguishable from a flat battery once it is installed.
    static uint32_t lastRadioTry = 0;
    if (!radioUp && now - lastRadioTry >= 5000) {
        lastRadioTry = now;
        radioUp = (loraBegin() == RADIOLIB_ERR_NONE);
        if (radioUp) {
            pinMode(LORA_DIO1, INPUT);
            ackTimeoutMs = loraTimeOnAirMs(sizeof(AckPacket)) + ACK_TURNAROUND_MS + 400;
            if (ackTimeoutMs < ACK_TIMEOUT_MS) ackTimeoutMs = ACK_TIMEOUT_MS;
            Serial.println("[radio] recovered");
        }
    }

    static uint32_t lastTx = 0;
    if (radioUp && now - lastTx >= (uint32_t)CFG.wake_secs * 1000) {
        lastTx = now;
        OcrResult r = lastResult;
        if (!previewing) {
            // Nothing else captured this cycle, so do it here.
            if (!measure(&r, false, false)) { Serial.println("[err] capture failed"); return; }
            reportOcr(r);
            lastResult = r;
        }
        transmitIfDue(r);
    }
    delay(2);
}
