#include "camera_hw.h"
#include "img_converters.h"

// XIAO ESP32S3 Sense camera pin map (Seeed wiki, "Camera Usage").
#define PWDN_GPIO_NUM  -1
#define RESET_GPIO_NUM -1
#define XCLK_GPIO_NUM  10
#define SIOD_GPIO_NUM  40
#define SIOC_GPIO_NUM  39
#define Y9_GPIO_NUM    48
#define Y8_GPIO_NUM    11
#define Y7_GPIO_NUM    12
#define Y6_GPIO_NUM    14
#define Y5_GPIO_NUM    16
#define Y4_GPIO_NUM    18
#define Y3_GPIO_NUM    17
#define Y2_GPIO_NUM    15
#define VSYNC_GPIO_NUM 38
#define HREF_GPIO_NUM  47
#define PCLK_GPIO_NUM  13

static uint32_t frameSeq = 0;
static uint32_t minFrameIntervalMs = 100;  // ~10 fps ceiling
static bool streaming = true;
static bool orientFlip = false;
static bool orientMirror = false;
static bool cameraOpen = false;

static void applyOrientation() {
    sensor_t *s = esp_camera_sensor_get();
    if (!s) return;
    s->set_vflip(s, orientFlip ? 1 : 0);
    s->set_hmirror(s, orientMirror ? 1 : 0);
}

void cameraSetOrientation(bool vflip, bool hmirror) {
    orientFlip = vflip;
    orientMirror = hmirror;
    applyOrientation();   // takes effect now if a sensor is live
}

bool cameraGetFlip()   { return orientFlip; }
bool cameraGetMirror() { return orientMirror; }

const char *cameraSensorName() {
    sensor_t *s = esp_camera_sensor_get();
    if (!s) return "none";
    switch (s->id.PID) {
        case 0x26:   return "OV2640";
        case 0x3660: return "OV3660";
        case 0x5640: return "OV5640";
        case 0x7725: return "OV7725";
        default:     return "unknown";
    }
}

uint16_t cameraSensorPid() {
    sensor_t *s = esp_camera_sensor_get();
    return s ? s->id.PID : 0;
}

bool cameraBegin() {
    camera_config_t c = {};
    c.ledc_channel = LEDC_CHANNEL_0;
    c.ledc_timer   = LEDC_TIMER_0;
    c.pin_d0 = Y2_GPIO_NUM;   c.pin_d1 = Y3_GPIO_NUM;
    c.pin_d2 = Y4_GPIO_NUM;   c.pin_d3 = Y5_GPIO_NUM;
    c.pin_d4 = Y6_GPIO_NUM;   c.pin_d5 = Y7_GPIO_NUM;
    c.pin_d6 = Y8_GPIO_NUM;   c.pin_d7 = Y9_GPIO_NUM;
    c.pin_xclk = XCLK_GPIO_NUM;
    c.pin_pclk = PCLK_GPIO_NUM;
    c.pin_vsync = VSYNC_GPIO_NUM;
    c.pin_href = HREF_GPIO_NUM;
    c.pin_sccb_sda = SIOD_GPIO_NUM;
    c.pin_sccb_scl = SIOC_GPIO_NUM;
    c.pin_pwdn = PWDN_GPIO_NUM;
    c.pin_reset = RESET_GPIO_NUM;
    c.xclk_freq_hz = 20000000;
    c.pixel_format = PIXFORMAT_JPEG;
    c.frame_size = FRAMESIZE_QVGA;   // 320x240; plenty for aiming
    c.jpeg_quality = 12;             // lower number = better quality, bigger frame
    c.fb_count = 2;
    c.fb_location = CAMERA_FB_IN_PSRAM;
    c.grab_mode = CAMERA_GRAB_LATEST;

    esp_err_t err = esp_camera_init(&c);
    if (err != ESP_OK) {
        Serial.printf("[cam] init failed: 0x%x\n", err);
        return false;
    }
    sensor_t *s = esp_camera_sensor_get();
    if (s) Serial.printf("[cam] sensor PID=0x%04X -> %s\n", s->id.PID, cameraSensorName());
    cameraOpen = true;
    applyOrientation();
    return true;
}

bool cameraBeginGray(int framesizeEnum) {
    camera_config_t c = {};
    c.ledc_channel = LEDC_CHANNEL_0;
    c.ledc_timer   = LEDC_TIMER_0;
    c.pin_d0 = Y2_GPIO_NUM;   c.pin_d1 = Y3_GPIO_NUM;
    c.pin_d2 = Y4_GPIO_NUM;   c.pin_d3 = Y5_GPIO_NUM;
    c.pin_d4 = Y6_GPIO_NUM;   c.pin_d5 = Y7_GPIO_NUM;
    c.pin_d6 = Y8_GPIO_NUM;   c.pin_d7 = Y9_GPIO_NUM;
    c.pin_xclk = XCLK_GPIO_NUM;
    c.pin_pclk = PCLK_GPIO_NUM;
    c.pin_vsync = VSYNC_GPIO_NUM;
    c.pin_href = HREF_GPIO_NUM;
    c.pin_sccb_sda = SIOD_GPIO_NUM;
    c.pin_sccb_scl = SIOC_GPIO_NUM;
    c.pin_pwdn = PWDN_GPIO_NUM;
    c.pin_reset = RESET_GPIO_NUM;
    c.xclk_freq_hz = 20000000;
    c.pixel_format = PIXFORMAT_GRAYSCALE;
    c.frame_size = (framesize_t)framesizeEnum;
    c.fb_count = 1;
    c.fb_location = CAMERA_FB_IN_PSRAM;
    c.grab_mode = CAMERA_GRAB_LATEST;

    esp_err_t err = esp_camera_init(&c);
    if (err != ESP_OK) {
        Serial.printf("[cam] grayscale init failed: 0x%x\n", err);
        return false;
    }
    cameraOpen = true;
    applyOrientation();
    return true;
}

bool cameraIsOpen() { return cameraOpen; }

void cameraEnd() {
    if (!cameraOpen) return;
    cameraOpen = false;
    // Deinit matters for power, not tidiness: users measure ~90 mA in deep
    // sleep on this board when the camera is left initialised, against ~2 mA
    // when it is not. It is still far from the 14 uA of a bare XIAO -- the
    // Sense board's regulators stay energised regardless -- so a hardware power
    // cut is still needed for multi-month life. See docs/hardware-notes.md.
    esp_camera_deinit();
    pinMode(XCLK_GPIO_NUM, INPUT);   // stop driving the 20 MHz clock
}

void cameraSendGrayPreview(const uint8_t *gray, int w, int h, int quality) {
    uint8_t *jpg = nullptr;
    size_t jpgLen = 0;
    if (!fmt2jpg((uint8_t *)gray, (size_t)w * h, w, h, PIXFORMAT_GRAYSCALE,
                 quality, &jpg, &jpgLen)) {
        Serial.println("[cam] preview encode failed");
        return;
    }
    Serial.printf(">>>FRAME %u %u %u %u\n", (unsigned)jpgLen, (unsigned)w,
                  (unsigned)h, (unsigned)frameSeq++);
    Serial.write(jpg, jpgLen);
    free(jpg);
}

// Resolution is selected by NAME, not by enum index: the framesize_t numbering
// has shifted between esp32-camera releases, so an index sent from the host
// could silently select the wrong size.
static bool framesizeByName(const String &name, framesize_t *out) {
    struct { const char *n; framesize_t v; } table[] = {
        {"QQVGA", FRAMESIZE_QQVGA},   // 160x120
        {"HQVGA", FRAMESIZE_HQVGA},   // 240x176
        {"QVGA",  FRAMESIZE_QVGA},    // 320x240
        {"CIF",   FRAMESIZE_CIF},     // 400x296
        {"VGA",   FRAMESIZE_VGA},     // 640x480
        {"SVGA",  FRAMESIZE_SVGA},    // 800x600
        {"XGA",   FRAMESIZE_XGA},     // 1024x768
    };
    for (auto &e : table) {
        if (name.equalsIgnoreCase(e.n)) { *out = e.v; return true; }
    }
    return false;
}

// One line per command:
//   CFG <NAME> <quality>   e.g. "CFG QVGA 12"
//   FLIP <0|1>   MIRROR <0|1>   FPS <n>   STREAM <0|1>
static void handleCommand(const String &line) {
    // Orientation is handled first: it is stored state, and must be settable
    // even while the camera is deinitialised between measurement cycles.
    if (line.startsWith("FLIP ")) {
        cameraSetOrientation(line.substring(5).toInt() != 0, orientMirror);
        Serial.printf("[cam] vflip %d\n", orientFlip ? 1 : 0);
        return;
    }
    if (line.startsWith("MIRROR ")) {
        cameraSetOrientation(orientFlip, line.substring(7).toInt() != 0);
        Serial.printf("[cam] hmirror %d\n", orientMirror ? 1 : 0);
        return;
    }
    sensor_t *s = esp_camera_sensor_get();
    if (!s) return;

    if (line.startsWith("CFG ")) {
        int sp = line.indexOf(' ', 4);
        String name = line.substring(4, sp > 0 ? sp : line.length());
        int q = sp > 0 ? line.substring(sp + 1).toInt() : -1;
        framesize_t fs;
        if (framesizeByName(name, &fs)) {
            s->set_framesize(s, fs);
        } else {
            Serial.printf("[cam] unknown framesize '%s'\n", name.c_str());
        }
        if (q >= 4 && q <= 63) s->set_quality(s, q);
        Serial.printf("[cam] cfg %s q=%d\n", name.c_str(), q);
    } else if (line.startsWith("FLIP ")) {
        cameraSetOrientation(line.substring(5).toInt() != 0, orientMirror);
        Serial.printf("[cam] vflip %d\n", orientFlip ? 1 : 0);
    } else if (line.startsWith("MIRROR ")) {
        cameraSetOrientation(orientFlip, line.substring(7).toInt() != 0);
        Serial.printf("[cam] hmirror %d\n", orientMirror ? 1 : 0);
    } else if (line.startsWith("FPS ")) {
        int fps = constrain((int)line.substring(4).toInt(), 1, 30);
        minFrameIntervalMs = 1000 / fps;
        Serial.printf("[cam] fps cap %d\n", fps);
    } else if (line.startsWith("STREAM ")) {
        streaming = line.substring(7).toInt() != 0;
        Serial.printf("[cam] streaming %s\n", streaming ? "on" : "off");
    }
}

void cameraPollCommandLine(const String &line) { handleCommand(line); }

void cameraPollCommands() {
    static String line;
    while (Serial.available()) {
        char ch = (char)Serial.read();
        if (ch == '\n' || ch == '\r') {
            if (line.length()) { handleCommand(line); line = ""; }
        } else if (line.length() < 64) {
            line += ch;
        }
    }
}

void cameraStreamTick() {
    static uint32_t lastFrame = 0;
    if (!streaming || millis() - lastFrame < minFrameIntervalMs) return;
    lastFrame = millis();

    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
        Serial.println("[cam] capture failed");
        return;
    }
    if (fb->format == PIXFORMAT_JPEG) {
        Serial.printf(">>>FRAME %u %u %u %u\n",
                      (unsigned)fb->len, (unsigned)fb->width,
                      (unsigned)fb->height, (unsigned)frameSeq++);
        Serial.write(fb->buf, fb->len);
    }
    esp_camera_fb_return(fb);
}
