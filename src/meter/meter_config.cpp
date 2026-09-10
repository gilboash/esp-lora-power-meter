#include <Preferences.h>
#include "meter_config.h"

MeterConfig CFG;
static Preferences prefs;

// Defaults are deliberately a whole-frame ROI: with nothing configured the
// preview shows everything, so the ROI can be drawn from what you can see
// rather than guessed at blind.
static const MeterConfig DEFAULTS = {
    /*roi_x*/ 0, /*roi_y*/ 0, /*roi_w*/ 0, /*roi_h*/ 0,
    /*digits*/ 6,
    /*wake_secs*/ 300,
    /*threshold*/ 0,
    /*invert*/ 0,
    /*lamp_ms*/ 120,
    /*heartbeat*/ 12,
    /*sleep_en*/ 0,
    /*flip*/ 0,
    // The OV3660 on this board delivers a horizontally mirrored image, so the
    // corrective default is mirror on rather than off.
    /*mirror*/ 1,
    /*rotate*/ 0,
};

void meterConfigLoad() {
    prefs.begin("meter", true);
    CFG = DEFAULTS;
    CFG.roi_x     = prefs.getUShort("rx", DEFAULTS.roi_x);
    CFG.roi_y     = prefs.getUShort("ry", DEFAULTS.roi_y);
    CFG.roi_w     = prefs.getUShort("rw", DEFAULTS.roi_w);
    CFG.roi_h     = prefs.getUShort("rh", DEFAULTS.roi_h);
    CFG.digits    = prefs.getUChar("dg", DEFAULTS.digits);
    CFG.wake_secs = prefs.getUShort("ws", DEFAULTS.wake_secs);
    CFG.threshold = prefs.getUChar("th", DEFAULTS.threshold);
    CFG.invert    = prefs.getUChar("iv", DEFAULTS.invert);
    CFG.lamp_ms   = prefs.getUShort("lm", DEFAULTS.lamp_ms);
    CFG.heartbeat = prefs.getUChar("hb", DEFAULTS.heartbeat);
    CFG.sleep_en  = prefs.getUChar("sl", DEFAULTS.sleep_en);
    CFG.flip      = prefs.getUChar("fl", DEFAULTS.flip);
    CFG.mirror    = prefs.getUChar("mi", DEFAULTS.mirror);
    CFG.rotate    = prefs.getUShort("ro", DEFAULTS.rotate);
    prefs.end();
}

void meterConfigSave() {
    prefs.begin("meter", false);
    prefs.putUShort("rx", CFG.roi_x);
    prefs.putUShort("ry", CFG.roi_y);
    prefs.putUShort("rw", CFG.roi_w);
    prefs.putUShort("rh", CFG.roi_h);
    prefs.putUChar("dg", CFG.digits);
    prefs.putUShort("ws", CFG.wake_secs);
    prefs.putUChar("th", CFG.threshold);
    prefs.putUChar("iv", CFG.invert);
    prefs.putUShort("lm", CFG.lamp_ms);
    prefs.putUChar("hb", CFG.heartbeat);
    prefs.putUChar("sl", CFG.sleep_en);
    prefs.putUChar("fl", CFG.flip);
    prefs.putUChar("mi", CFG.mirror);
    prefs.putUShort("ro", CFG.rotate);
    prefs.end();
}

void meterConfigPrint() {
    Serial.printf("[cfg] roi=%u,%u,%ux%u digits=%u wake=%us thr=%u inv=%u lamp=%ums hb=%u sleep=%u flip=%u mirror=%u rot=%u\n",
                  CFG.roi_x, CFG.roi_y, CFG.roi_w, CFG.roi_h, CFG.digits,
                  CFG.wake_secs, CFG.threshold, CFG.invert, CFG.lamp_ms, CFG.heartbeat, CFG.sleep_en, CFG.flip, CFG.mirror, CFG.rotate);
}

bool meterConfigCommand(const String &line) {
    if (!line.startsWith("SET ")) return false;
    int sp = line.indexOf(' ', 4);
    if (sp < 0) return false;
    String key = line.substring(4, sp);
    long v = line.substring(sp + 1).toInt();

    if      (key == "roi_x")  CFG.roi_x = v;
    else if (key == "roi_y")  CFG.roi_y = v;
    else if (key == "roi_w")  CFG.roi_w = v;
    else if (key == "roi_h")  CFG.roi_h = v;
    else if (key == "digits") CFG.digits = constrain(v, 1, 12);
    else if (key == "wake")   CFG.wake_secs = constrain(v, 5, 3600);
    else if (key == "thr")    CFG.threshold = constrain(v, 0, 255);
    else if (key == "invert") CFG.invert = v ? 1 : 0;
    else if (key == "lamp")   CFG.lamp_ms = constrain(v, 0, 2000);
    else if (key == "hb")     CFG.heartbeat = constrain(v, 1, 255);
    else if (key == "sleep")  CFG.sleep_en = v ? 1 : 0;
    else if (key == "flip")   CFG.flip = v ? 1 : 0;
    else if (key == "mirror") CFG.mirror = v ? 1 : 0;
    else if (key == "rotate") {
        long r = ((v % 360) + 360) % 360;
        CFG.rotate = (r == 90 || r == 180 || r == 270) ? (uint16_t)r : 0;
    }
    else return false;

    meterConfigSave();
    meterConfigPrint();
    return true;
}
