# Hardware notes

Findings from verifying the brief's open risks before writing firmware. Each pin
number below is traced to a source, not inferred from a photo.

## 1. The camera node's module choice is not just convenient — it is required

The brief listed "camera daughterboard + Wio-SX1262 coexistence" as mechanically
inferred but unconfirmed. It is confirmed, and the reason is electrical rather
than mechanical:

| | Wio-SX1262 attach | SX1262 control pins |
|---|---|---|
| Bundled kit module | 30-pin board-to-board | GPIO 38, 39, 40, 41, 42 |
| Standalone SKU 113010003 (pin header) | 14-pin edge header | GPIO 2, 3, 4, 5, 6 |

The XIAO ESP32S3 **Sense** routes its camera over GPIO 10–18 and 38–48, including
**VSYNC on GPIO38, SCCB clock on GPIO39 and SCCB data on GPIO40**. Those are
exactly three of the five control pins the B2B kit module uses.

So: the bundled kit module and the Sense camera **cannot** coexist — they would
fight over the same three GPIOs. The standalone pin-header module lands on
GPIO 2–6, which the camera never touches. Pairing unit #3 with unit #2 (as the
brief planned) is the only combination that works, and pairing unit #1's module
with the Sense would not have.

Still worth the bench stack check for physical clearance, but there is no pin
conflict to discover.

## 2. Pin maps (both nodes)

From Meshtastic's own variant files, which are the maintained source for these
exact modules.

**Camera node** — XIAO ESP32S3 Sense + standalone Wio-SX1262 (pin header).
Meshtastic's nRF52840 variant.h carries a pin table that names
`standalone SKU 113010003` explicitly, in XIAO silkscreen labels:

| Signal | XIAO pin | ESP32-S3 GPIO |
|---|---|---|
| CS | D4 | 5 |
| DIO1 | D1 | 2 |
| BUSY | D3 | 4 |
| RESET | D2 | 3 |
| RXEN | D5 | 6 |
| SCK / MISO / MOSI | D8 / D9 / D10 | 7 / 8 / 9 |

**Gateway node** — plain XIAO ESP32S3 + bundled kit module (B2B):

| Signal | GPIO |
|---|---|
| CS | 41 |
| DIO1 | 39 |
| BUSY | 40 |
| RESET | 42 |
| RXEN | 38 |
| SCK / MISO / MOSI | 7 / 8 / 9 |

Both modules use a **TCXO on DIO3 at 1.8 V** and **DIO2 as the TX-side RF
switch**, with RXEN as a separate GPIO. Missing either of those settings does not
fail loudly — it just quietly destroys range, which would poison the walk test.

## 3. Meshtastic vs custom firmware — resolved

The brief left this open. Recommendation: **custom firmware**, for two
independent reasons.

*Support:* Meshtastic ships variants for XIAO ESP32S3 + B2B module, and for XIAO
nRF52840 + pin-header module. The camera node's combination — ESP32S3 + pin-header
module — is neither. [Issue #6478](https://github.com/meshtastic/firmware/issues/6478)
requested exactly it and was **closed as not planned**. It is reachable by writing
a custom variant (the issue author did), but that is custom firmware work anyway.

*Fit:* the stronger reason. Meshtastic is an always-on mesh router. The camera
node's entire power budget rests on sleeping for minutes at a time, waking, and
transmitting only on change — the opposite duty cycle. Meshtastic also has no
place to host camera capture and digit recognition. Even with perfect hardware
support it would be the wrong shape.

The mesh-fallback argument stays available: if the walk test shows a marginal
link, the cheapest fix is a third node, and the framing in `link_config.h` is
small enough to relay without protocol work.

## 4. Things that will matter in later build steps

- **The LoRa module shares the SPI bus with the Sense's microSD** (GPIO 7/8/9,
  SD CS on GPIO21). Fine now — nothing else is on the bus during the walk test —
  but when image buffering to SD arrives, both must share one `SPIClass`.
- **SD CS and the user LED are the same pin (GPIO21).** The walk test uses it as
  the LED. They cannot both be live.
- **GPIO10 carries a 20 MHz camera XCLK while the camera is open**, and on some
  units couples into the SD SPI lines and causes CRC errors. If SD writes turn
  flaky in step 4, this is the first suspect.
- **No on-board battery divider.** Unlike some XIAO boards, the ESP32S3 has none,
  so `meter/camera/battery` needs an external divider (e.g. 2× 220 kΩ) into
  **D0 / GPIO1**, which is free on both nodes. Firmware support is written and
  gated behind `HAS_BATTERY_DIVIDER`, defaulting off so it reports 0 rather than
  a fabricated voltage. Prefer a divider you can disconnect, or its own quiescent
  drain eats into the multi-month target.
- **GPIO3 (D2, the LoRa RESET) is an ESP32-S3 strapping pin.** Driven by the MCU
  it is harmless, but never add a strong external pull-up.
- **RadioLib warns that USB CDC serial output can stop after the first sleep.**
  Expect the serial log to go quiet once deep sleep lands in step 6; plan on the
  MQTT diagnostics, not the USB console, as the telemetry path by then.

## Sources

- [Meshtastic firmware — `variants/nrf52840/seeed_xiao_nrf52840_kit/variant.h`](https://raw.githubusercontent.com/meshtastic/firmware/master/variants/nrf52840/seeed_xiao_nrf52840_kit/variant.h) (pin table naming SKU 113010003)
- [Meshtastic firmware — `variants/esp32s3/seeed_xiao_s3/variant.h`](https://raw.githubusercontent.com/meshtastic/firmware/master/variants/esp32s3/seeed_xiao_s3/variant.h)
- [meshtastic/firmware issue #6478 — Seeed Xiao ESP32S3 + SX1262 for XIAO (not the kit)](https://github.com/meshtastic/firmware/issues/6478)
- [Meshtastic docs — Seeed Wio-SX1262 series](https://meshtastic.org/docs/hardware/devices/community-supported/seeed-studio/wio-series/wio-sx1262s/)
- [Seeed wiki — Camera usage on XIAO ESP32S3 Sense](https://wiki.seeedstudio.com/xiao_esp32s3_camera_usage/)
- [Seeed wiki — How to check the battery voltage](https://wiki.seeedstudio.com/check_battery_voltage/)
- [XIAO ESP32S3 Sense pinout and specifications](https://www.espboards.dev/esp32/xiao-esp32s3-sense/)

---

# Field findings (walk test + meter survey)

## 5. The link works single-hop, but not at SF9

Measured floor 5 down to the meter closet on level −1:

| Location | RSSI | SNR | margin at SF9 (limit −12.5 dB) |
|---|---|---|---|
| Floor 4 | −44 dBm | +11.3 dB | ample |
| Floor 2 | −76 dBm | +9.0 dB | ample |
| Floor −1 | −101 dBm | −10.0 dB | **2.5 dB** |
| −1, in closet | −92 dBm | −4.8 dB | 7.7 dB |

94% delivery over the whole walk. RSSI alone looks comfortable, but **SNR is the
binding constraint underground** — the signal is below the noise floor, which
LoRa tolerates only down to its per-SF demodulation limit.

`LORA_PROFILE_ROBUST` (SF12) decodes to −20 dB, roughly 7.5 dB more margin, at a
cost of ~1 s airtime for a 12-byte reading. That is irrelevant at a wake interval
of minutes. **The meter node ships at SF12** (`env:meter_node`); the walk-test
envs stay at SF9 so the two are not confused.

## 6. Deep sleep on the Sense board undermines the battery target

Community measurements on this exact board:

- bare XIAO ESP32S3, no Sense: **~14 µA**
- Sense attached, camera never initialised: **~2 mA**
- Sense attached, camera initialised and not deinitialised: **~90 mA**

Seeed's advertised 3.8 mA figure was obtained under ESP-IDF with peripherals
explicitly shut down, which the Arduino layer does not expose. `cameraEnd()`
calls `esp_camera_deinit()` and stops driving XCLK, which avoids the 90 mA case,
but ~2 mA is the practical floor while the Sense board is powered.

At 2 mA a 1000 mAh cell lasts about 20 days before a single wake is counted.

**This inverts a premise in the brief.** Per wake, camera capture costs roughly
250 mA·s against ~60 mA·s for an SF12 transmit — so the *camera*, not the radio,
dominates, and change-detection gating saves less than expected. The levers that
actually matter, in order:

1. **Wake interval.** 5 min → ~288 wakes/day; 15 min → ~96. Linear.
2. **Cutting power to the board between wakes.** An external latching timer
   (TPL5110 class) pulls the whole node to ~2 µA between wakes; the MCU pulls
   DONE when finished. This is what turns weeks into months.
3. Battery size.

## 7. The meter: ISKRA MT171-D2A54-K0

Three-phase, IEC 62053-21, Israel Electric Corporation, 2009.

- **The LCD is reflective and unlit.** It is blank in a dark closet, so the node
  must bring its own light. `LAMP_PIN` (D6 / GPIO43) is reserved for it.
- **Two layers of glare.** The meter sits behind plastic sheeting *and* its own
  clear cover. Mount the LED **off-axis** — a coaxial light reflects straight
  back into the lens and washes out the digits. Two LEDs at ~45° from opposite
  sides, or one side light plus a diffuser.
- **`500 imp/kWh` pulse LED on the faceplate.** This is a standard optical output
  flashing once per 2 Wh. Counting it with a phototransistor needs no camera, no
  illumination and no OCR, and would let the node drop the Sense board entirely
  (~14 µA sleep instead of ~2 mA). It yields increments rather than an absolute
  reading, which Home Assistant's `utility_meter` handles from a one-time seed.
  Worth considering as either a replacement for, or a cross-check on, the camera.
- **Open question:** whether the display auto-scrolls through registers (total,
  tariffs, phase data). If it cycles, the node must recognise *which* screen it
  is looking at before trusting the digits — the indicator row beneath the
  display (`L1 L2 L3  RST DRO FF`) is the likely discriminator.
