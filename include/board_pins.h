#pragma once

// ---------------------------------------------------------------------------
// XIAO ESP32S3 silkscreen label -> ESP32-S3 GPIO number.
// Identical on the plain XIAO ESP32S3 and the Sense; the Sense only adds the
// B2B expansion board (camera/mic/SD) underneath.
// ---------------------------------------------------------------------------
#define XIAO_D0   1   // A0 - free on both nodes, used here for battery sense
#define XIAO_D1   2   // A1
#define XIAO_D2   3   // A2  (ESP32-S3 strapping pin, see note below)
#define XIAO_D3   4   // A3
#define XIAO_D4   5   // SDA
#define XIAO_D5   6   // SCL
#define XIAO_D6   43  // TX
#define XIAO_D7   44  // RX
#define XIAO_D8   7   // SCK
#define XIAO_D9   8   // MISO
#define XIAO_D10  9   // MOSI

// SPI bus is the same on both nodes: the standard XIAO SPI pins.
#define LORA_SCK   XIAO_D8   // GPIO7
#define LORA_MISO  XIAO_D9   // GPIO8
#define LORA_MOSI  XIAO_D10  // GPIO9

#if defined(BOARD_CAMERA_NODE)
// ---------------------------------------------------------------------------
// Camera node: XIAO ESP32S3 Sense + "Wio-SX1262 for XIAO", the standalone
// pin-header module (SKU 113010003 / same PCB as the nRF52840 kit SKU 102010710).
//
// Source of truth: meshtastic/firmware variants/nrf52840/seeed_xiao_nrf52840_kit/
// variant.h, whose pin table names SKU 113010003 explicitly ("Default" column):
//     D1=DIO1  D2=NRST  D3=BUSY  D4=CS  D5=RXEN  D8/D9/D10=SCK/MISO/MOSI
// Translated here from XIAO silkscreen labels to ESP32-S3 GPIOs.
//
// Why this module and not the B2B one on the Sense: the B2B kit module lives on
// GPIO 38-42, which is exactly where the Sense routes the camera (VSYNC 38,
// SIOC 39, SIOD 40). The pin-header module sits on GPIO 2-6 instead and so does
// not collide with the camera at all. See docs/hardware-notes.md.
// ---------------------------------------------------------------------------
#define LORA_CS    XIAO_D4   // GPIO5
#define LORA_DIO1  XIAO_D1   // GPIO2
#define LORA_BUSY  XIAO_D3   // GPIO4
#define LORA_RST   XIAO_D2   // GPIO3  (strapping pin; driven by MCU, harmless
                             //         after boot, but never add a strong pull-up)
#define LORA_RXEN  XIAO_D5   // GPIO6  RX side of the RF switch; TX side is DIO2

#define NODE_NAME "camera"

#elif defined(BOARD_GATEWAY_NODE) || defined(BOARD_PULSE_NODE)
// ---------------------------------------------------------------------------
// Both nodes carrying the 30-pin board-to-board Wio-SX1262 from the bundled
// kit, which sits on GPIO 38-42:
//
//   BOARD_GATEWAY_NODE  plain XIAO ESP32S3 by the router, bridges to the host
//   BOARD_PULSE_NODE    plain XIAO ESP32S3 at the meter, counts its pulse LED
//
// Confirmed per board with the diag probe rather than assumed: the two module
// variants look similar and the pinouts share nothing, so a wrong guess gives
// CHIP_NOT_FOUND and no other clue.
//
// Source of truth: meshtastic/firmware variants/esp32s3/seeed_xiao_s3/variant.h
// ---------------------------------------------------------------------------
#define LORA_CS    41
#define LORA_DIO1  39
#define LORA_BUSY  40
#define LORA_RST   42
#define LORA_RXEN  38

#if defined(BOARD_PULSE_NODE)
#define NODE_NAME "pulse"

// ---------------------------------------------------------------------------
// TEPT4400 phototransistor watching the meter's optical pulse output.
//
//     D0 (GPIO1) ---- collector
//     GND        ---- emitter
//
// No external resistor: the pin's internal ~45k pull-up is the load. Dark
// leaves the transistor off and the pin held high; light saturates it
// (VCEsat 0.1 V) and drags the pin to ground. So a pulse is a FALLING edge.
// Verified on the bench: covered reads 1 / 2521 mV, lit reads 0 / 0 mV.
//
// Dark current is 350 nA against a saturated pulse, a margin of about 100x, so
// sensitivity is never the problem. Stray light is: room lighting alone pins
// the input low, which is why the sensor must be shrouded and pressed against
// the meter's LED rather than merely pointed at it.
//
// GPIO1 is clear of the B2B module (GPIO 38-42 plus the SPI bus) and is also
// ADC1_CH0, so the raw level can be sampled to check aim and shrouding.
// ---------------------------------------------------------------------------
#define PULSE_PIN  XIAO_D0   // GPIO1
#else
#define NODE_NAME "gateway"
#endif

#else
#error "Define BOARD_CAMERA_NODE or BOARD_GATEWAY_NODE (see platformio.ini)"
#endif

// ---------------------------------------------------------------------------
// On-board user LED. Active LOW on the XIAO ESP32S3.
// NOTE: on the Sense this same GPIO is the microSD chip select. Fine during the
// walk test (no SD in use); revisit when SD logging is added.
// ---------------------------------------------------------------------------
#define USER_LED   21
#define LED_ON     LOW
#define LED_OFF    HIGH

// Battery sense: the XIAO ESP32S3 has NO on-board battery divider, unlike some
// other XIAO boards. Reading is only meaningful after fitting an external
// divider to D0 (see docs/hardware-notes.md). Gated by HAS_BATTERY_DIVIDER.
// ---------------------------------------------------------------------------
// Illumination for the meter's reflective LCD, which has no backlight.
// D6 is free on this node (UART TX, unused because logging goes over USB CDC).
// Driven by PWM at 20 kHz, active high: duty 0 is off, 255 is full brightness.
// The carrier is kept well above the camera's row readout so the rolling
// shutter cannot turn it into banding across the frame.
//
// Wiring, as verified on the LilyPad-style modules actually in use here:
//
//     D6 (GPIO43) ---- pad silk-screened "-"
//     GND         ---- pad silk-screened "+"
//
// That reads backwards and is correct: the silkscreen on these clones does not
// match the pads, so wiring by the markings gives an LED that never lights and
// shows no other symptom. Electrically D6 remains the anode side and the pin
// remains active high. The modules carry their own series resistor, so no
// external one is needed.
//
// One or two modules can hang directly off the pin (~4-5 mA each at 3V3, inside
// the GPIO's limit). For three or more, or to run them from BAT+ for more
// light, switch them with an NPN: GPIO43 -> 1k -> base of a 2N3904, emitter to
// GND, the modules' "-" pads to the collector.
//
// Mount OFF-AXIS. The meter sits behind plastic sheeting and its own clear
// cover, so a light beside the lens reflects straight back into it. Aim across
// the display from roughly 45 degrees, and expect to tune the brightness DOWN.
// ---------------------------------------------------------------------------
#define LAMP_PIN         XIAO_D6   // GPIO43
#define LAMP_ON          HIGH
#define LAMP_OFF         LOW

#define VBAT_ADC_PIN     XIAO_D0
#define VBAT_DIVIDER     2.0f   // 2:1 divider (e.g. 2x 220k) -> adjust to yours
