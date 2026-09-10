#pragma once
#include <Arduino.h>
#include "esp_camera.h"

// Camera bring-up and USB frame streaming for the XIAO ESP32S3 Sense.
// Shared by the USB-only streamer and the combined LoRa + camera node, so the
// pin map and wire format live in exactly one place.

bool cameraBegin();               // JPEG mode, for the live viewfinder
// Grayscale mode, for measurement: OCR wants raw pixels, and decoding a JPEG on
// the node just to threshold it would waste both time and power.
bool cameraBeginGray(int framesizeEnum);
void cameraEnd();                 // deinit; call before deep sleep (see notes)

// Orientation must outlive the sensor. measure() re-inits and deinits the
// camera every cycle, so a set_hmirror() applied to the live sensor is gone by
// the next frame -- and does nothing at all if called while deinitialised.
// These are kept here and re-applied immediately after each init.
void cameraSetOrientation(bool vflip, bool hmirror);
bool cameraGetFlip();
bool cameraGetMirror();
bool cameraIsOpen();              // true between cameraBegin*/cameraEnd

// Encode an 8-bit grayscale buffer and emit it using the same >>>FRAME format,
// so the dashboard renders measurement frames through the existing path.
void cameraSendGrayPreview(const uint8_t *gray, int w, int h, int quality);
const char *cameraSensorName();   // e.g. "OV3660", resolved from the sensor PID
uint16_t cameraSensorPid();

// Consume any pending host commands (CFG / FLIP / MIRROR / FPS / STREAM).
void cameraPollCommands();
// Handle one already-assembled command line (the meter node owns its own
// serial reader, because it must also parse SET commands).
void cameraPollCommandLine(const String &line);

// Emit one frame if streaming is enabled and the fps cap allows it.
// Wire format, so binary frames can share the stream with text logs:
//     >>>FRAME <bytes> <width> <height> <seq>\n
//     <bytes raw JPEG>
void cameraStreamTick();
