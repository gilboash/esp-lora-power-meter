// ---------------------------------------------------------------------------
// USB-only camera streamer for the XIAO ESP32S3 Sense.
//
// No radio: useful when you just want a viewfinder (aiming the lens, checking
// focus and framing on the meter) without the LoRa duty cycle in the way.
// For the combined node that also transmits, build env `camera_node`.
// ---------------------------------------------------------------------------
#include <Arduino.h>
#include "camera_hw.h"

void setup() {
    Serial.begin(115200);
    uint32_t t0 = millis();
    while (!Serial && millis() - t0 < 3000) delay(10);

    Serial.println("\n=== XIAO ESP32S3 Sense :: USB camera stream ===");
    Serial.printf("[cam] PSRAM: %s (%u bytes free)\n",
                  psramFound() ? "yes" : "NO - camera will fail",
                  (unsigned)ESP.getFreePsram());

    if (!cameraBegin()) {
        Serial.println("[fatal] camera init failed. Check the FPC ribbon seating.");
        while (true) delay(1000);
    }
    Serial.println("[cam] ready, streaming");
}

void loop() {
    cameraPollCommands();
    cameraStreamTick();
    delay(2);
}
