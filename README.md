# LoRa Power Meter

Battery camera node photographs a digital electricity meter on the parking level
and relays the reading over 868 MHz LoRa to a mains-powered gateway upstairs,
which republishes to Mosquitto for Home Assistant. No WiFi at the meter.

Full design brief: see the project brief. Hardware findings and pin provenance:
[`docs/hardware-notes.md`](docs/hardware-notes.md).

## Status

Build order from the brief, and where it stands:

| # | Step | Status |
|---|---|---|
| 1 | Bench mechanical check | **done** — both boards assembled and verified |
| 2 | LoRa link walk test | **done** — floor 5 to level −1, 94% delivery |
| 3 | Camera capture on the bench | **done** — OV3660 confirmed, live view over USB |
| 4 | Capture → decode → transmit | **done** — readings arrive at the gateway over LoRa |
| 5 | Gateway MQTT bridge | not started |
| 6 | Power optimisation pass | not started (see the deep-sleep finding) |
| 7 | On-device digit recognition | **working** — validated against a real display |
| 8 | Enclosure and install | not started |

Not yet done on the real meter: it decodes a seven-segment display correctly on
the bench, but the ISKRA's LCD is unlit and behind plastic, so it still needs an
LED on `LAMP_PIN` and a ROI calibrated against the real thing.

Two of the brief's open risks are now closed:

- **Camera + LoRa coexistence.** Confirmed, and the reason turned out to be
  electrical, not mechanical: the bundled kit's B2B module sits on GPIO 38–42,
  which is where the Sense routes camera VSYNC/SCCB. The standalone pin-header
  module sits on GPIO 2–6 and does not collide. Pairing unit #3 with the Sense is
  the only workable combination — the other way round could never have worked.
- **Firmware approach.** Custom, not Meshtastic. Meshtastic has no variant for
  ESP32S3 + pin-header module and [declined to add one](https://github.com/meshtastic/firmware/issues/6478),
  but the deciding reason is fit: Meshtastic is an always-on mesh router, and the
  camera node's power budget depends on sleeping through most of its life.

Still open, and only the walk test can close it: **real link performance through
this building's concrete.**

## Layout

```
platformio.ini            build environments, one per node role and mode
include/board_pins.h      pin maps for both nodes, each traced to a source
include/link_config.h     RF profile, duty-cycle limit, wire format
include/lora_radio.h      shared radio bring-up
src/lora_radio.cpp        SPI + SX1262 init (TCXO, RF switch, CRC)
src/walktest/tx_main.cpp  walk-test TX: ping, await ACK, report on LED
src/walktest/rx_main.cpp  gateway: receive pings and readings, ACK, log CSV
src/diag/spi_probe.cpp    SX1262 hardware probe (float test + header scan)
src/camera/camera_hw.cpp  camera bring-up, orientation, USB frame streaming
src/meter/seven_seg.cpp   seven-segment decoder
src/meter/meter_config.cpp  NVS-backed ROI / interval / orientation settings
src/node/meter_node_main.cpp  the meter node: capture, decode, transmit
tools/walktest_server.py  dashboard: link status, live camera, ROI picker
tools/ocr_sim.py          the decoder in Python, for offline tuning
tools/find_port.py        resolve a board by USB serial, not by /dev name
tests/native/test_ocr.cpp host-side regression test for the decoder
tests/fixtures/           real captures the decoder is tested against
docs/hardware-notes.md    findings, conflicts, sources
```

## Build environments

| Env | Board | Purpose |
|---|---|---|
| `meter_node` | Sense | the real node: capture, decode, transmit (SF12) |
| `meter_gateway` | plain | its gateway — **must match the profile** (SF12) |
| `walktest_camera` / `walktest_gateway` | both | range diagnostics (SF9), used as a pair |
| `camera_stream` | Sense | viewfinder only, no radio |
| `diag_camera` / `diag_gateway` | either | SX1262 probe when a radio will not init |

Spreading factors must match between a node and its gateway — mismatched nodes
are completely deaf to each other, with nothing in either log to say why.

## Dashboard

```sh
~/.platformio/penv/bin/python tools/walktest_server.py     # http://localhost:8420
```

Serves link status, readings, the RSSI chart, and a Camera tab with a live view,
ROI picker, threshold view and per-segment decoder diagnostics. Camera preview is
opt-in so that attaching USB does not change how the node behaves.

## Testing the decoder without hardware

```sh
python3 tools/ocr_sim.py tests/fixtures/iron_290_1.jpg --expect 290
g++ -std=c++17 -O2 -Iinclude -Itests/native \
    src/meter/seven_seg.cpp tests/native/test_ocr.cpp -o /tmp/test_ocr && /tmp/test_ocr
```

## Running the walk test

Flash each node from its own environment — they are not interchangeable, since
the LoRa module attaches on a different connector on each board.

```sh
# gateway: plain XIAO ESP32S3 + bundled kit module
pio run -e walktest_gateway -t upload
pio device monitor -e walktest_gateway

# camera node: XIAO ESP32S3 Sense + standalone pin-header module
pio run -e walktest_camera -t upload
```

Leave the gateway on USB next to the HA host. Carry the camera node on a USB
power bank down to the meter.

**The camera node reports link quality on its own LED**, so you can walk the
building without a laptop:

| LED | Meaning |
|---|---|
| 1 short blink | uplink RSSI better than −100 dBm — strong |
| 2 short blinks | −100 to −115 dBm — usable |
| 3 short blinks | worse than −115 dBm — marginal |
| 1 long blink | no ACK — link down |

The ping is acknowledged, so both directions are measured: the ACK carries the
RSSI/SNR the *gateway* saw, and the camera node adds what it saw on the way back.
A one-way test would have hidden an asymmetric link.

Both nodes print CSV to serial. The gateway also counts sequence gaps, so packet
delivery ratio is measured at the receiver rather than inferred from missing
ACKs, and prints a rolling summary every 30 s:

```
[summary] rx=142 missed=3 bad=0 pdr=97.9% rssi min/avg/max = -118.0/-103.4/-91.5 dBm
```

### Reading the result

- **PDR above ~95% with RSSI better than −115 dBm** — single hop is fine, proceed
  to step 3.
- **Marginal** — switch both nodes to `-DLORA_PROFILE_ROBUST` in `platformio.ini`
  (SF12/CR4:8, roughly 6 dB more link budget for much slower packets — irrelevant
  for a payload this small) and retest before considering a relay node.
- **Nothing received at all** — check the pin map first. Getting the RF switch or
  TCXO settings wrong does not fail loudly, it just destroys range.

### Radio settings

868.1 MHz, 14 dBm, SF9/BW125/CR4:5 by default, private sync word `0x12`.
Israel's SRD allocation follows the CEPT plan for this band, so the EU limits are
applied: 25 mW and a 1% duty cycle. The transmitter measures its own time on air
at boot and **raises its ping interval automatically** if the configured 5 s
would exceed 1%, so changing the profile cannot silently put you over the limit.
Confirm current MoC rules before a permanent install.

## Notes for the next step

`docs/hardware-notes.md` records several things that will bite in steps 3–6 —
the SD card sharing the LoRa SPI bus, SD chip select colliding with the user LED,
the camera's 20 MHz XCLK coupling into SD lines, the absent battery divider, and
RadioLib's warning that USB CDC serial can stop after the first deep sleep.
