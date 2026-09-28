#!/usr/bin/env python3
"""Live walk-test dashboard + USB camera view.

Reads whatever is plugged in over USB:
  * the gateway's walk-test CSV -> live link stats, RSSI chart, markers
  * the camera node's JPEG frames -> live MJPEG view for aiming the lens

Bound to all interfaces, so the page can be opened on a phone over WiFi while
carrying the camera node around the building.

    python3 tools/walktest_server.py

Nothing is stored on disk unless you hit Export, so a session is disposable.
"""

import csv
import glob
import io
import json
import os
import re
import socket
import threading
import time
from collections import deque
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

try:
    import serial
except ImportError:
    raise SystemExit(
        "pyserial is missing. Run it with PlatformIO's python, which already has it:\n"
        "  ~/.platformio/penv/bin/python tools/walktest_server.py"
    )

PORT = int(os.environ.get("WALKTEST_PORT", "8420"))
BAUD = 115200
MAX_PACKETS = 5000
HERE = os.path.dirname(os.path.abspath(__file__))

# Matches both node's CSV rows: a leading integer sequence number, then numbers,
# with empty fields allowed (the camera node leaves RSSI blank on a missed ACK).
ROW_RE = re.compile(r"^\d+(,[-\d.]*)+$")
# Emitted by src/camera/stream_main.cpp, followed by exactly <bytes> raw JPEG.
FRAME_RE = re.compile(r"^>>>FRAME (\d+) (\d+) (\d+) (\d+)$")
OCR_RE = re.compile(r"^\[ocr\] value=(\d+) ok=(\d) conf=(\d+) thr=(\d+) segs=(\S*)$")
CFG_RE = re.compile(r"^\[cfg\] roi=(\d+),(\d+),(\d+)x(\d+) digits=(\d+) wake=(\d+)s "
                    r"thr=(\d+) inv=(\d+) lamp=(\d+)ms(?: bright=(\d+))?(?: hold=(\d+))?(?: hb=(\d+))?(?: sleep=(\d+))?(?: flip=(\d+))?(?: mirror=(\d+))?(?: rot=(\d+))?(?: fine=(-?\d+))?(?: ap=(\d+))?$")
# Emitted by the gateway when a decoded meter reading arrives over LoRa.
ROI_RE = re.compile(r"^\[roi\] min=(\d+) mean=(\d+) max=(\d+) contrast=(\d+) thr=(\d+) "
                    r"trim=(\d+),(\d+) (\d+)x(\d+)(?: edge=(\d+))?$")
SEG_RE = re.compile(r"^\[seg\] d(\d+):((?: [a-g]=\d+)+)\s+-> (.)$")
READING_RE = re.compile(r"^READING,(\d+),(\d+),(\d+),(\d+),([-\d.]+),([-\d.]+),(\d+)(?:,(\d))?$")


class State:
    def __init__(self):
        self.lock = threading.Lock()
        self.packets = []        # gateway-side receptions
        self.markers = []        # user-placed location labels
        self.ports = {}          # port -> {"role": str, "connected": bool, "last": float}
        self.radio = {}          # role -> banner line
        self.started = time.time()
        self.frame = None        # {"jpeg": bytes, "w","h","seq","t"}
        self.frame_times = deque(maxlen=30)
        self.frame_count = 0
        self.serials = {}        # role -> (serial, write-lock), for sending commands
        # Uploading firmware needs exclusive access to the port, and this server
        # holds it. Rather than stopping the server for every flash (and then
        # forgetting to restart it), readers release their ports while paused.
        self.pause_until = 0.0
        self.ocr = None          # newest local decode, from the node over USB
        self.cfg = None          # node's persisted ROI / interval settings
        self.diag = None         # [roi] contrast/threshold/trim from the decoder
        self.segs = {}           # digit index -> per-segment fill percentages
        self.readings = []       # decoded readings that arrived over LoRa

    def set_frame(self, jpeg, w, h, seq):
        with self.lock:
            self.frame = {"jpeg": jpeg, "w": w, "h": h, "seq": seq, "t": time.time()}
            self.frame_times.append(time.time())
            self.frame_count += 1

    def camera_status(self):
        with self.lock:
            f, times = self.frame, list(self.frame_times)
            n = self.frame_count
        fps = 0.0
        if len(times) >= 2:
            span = times[-1] - times[0]
            if span > 0:
                fps = (len(times) - 1) / span
        if not f:
            return {"live": False, "fps": 0.0, "frames": n}
        return {
            "live": (time.time() - f["t"]) < 6.0,
            "fps": round(fps, 1), "frames": n,
            "w": f["w"], "h": f["h"], "seq": f["seq"],
            "bytes": len(f["jpeg"]), "age": time.time() - f["t"],
        }

    def send(self, command):
        """Write a command line to the camera node, if it is connected."""
        with self.lock:
            entry = self.serials.get("camera")
        if not entry:
            return False
        ser, wlock = entry
        try:
            with wlock:
                ser.write((command.strip() + "\n").encode())
            return True
        except Exception:
            return False

    def snapshot(self):
        with self.lock:
            recent = self.packets[-240:]
            last = self.packets[-1] if self.packets else None
            received = len(self.packets)
            missed = last["missed"] if last else 0
            total = received + missed
            return {
                "now": time.time(),
                "started": self.started,
                "packets": recent,
                "total_received": received,
                "total_missed": missed,
                "pdr": (100.0 * received / total) if total else None,
                "last": last,
                "markers": list(self.markers),
                "ports": dict(self.ports),
                "radio": dict(self.radio),
                "ocr": self.ocr,
                "diag": self.diag,
                "segs": [self.segs[k] for k in sorted(self.segs)] if self.segs else [],
                "cfg": self.cfg,
                "readings": self.readings[-60:],
            }


STATE = State()


def parse_gateway_row(fields):
    # seq,rssi,snr,freq_err,vbat_mv,rx_ok,missed,pdr%
    def num(i, cast=float):
        try:
            return cast(fields[i])
        except (ValueError, IndexError):
            return None
    return {
        "t": time.time(),
        "seq": num(0, int),
        "rssi": num(1),
        "snr": num(2),
        "ferr": num(3),
        "vbat": num(4, int),
        "rx_ok": num(5, int),
        "missed": num(6, int) or 0,
        "pdr": num(7),
    }


def reader(port):
    """Own one serial port. Survives the board being unplugged mid-walk.

    The stream carries text log lines and, from the camera node, length-prefixed
    binary JPEG frames. `pending` holds the frame header while its payload is
    still arriving, so a frame that spans several reads is reassembled rather
    than mistaken for text.
    """
    while True:
        if time.time() < STATE.pause_until:
            time.sleep(0.5)
            continue
        try:
            ser = serial.Serial(port, BAUD, timeout=0.4)
        except Exception:
            with STATE.lock:
                if port in STATE.ports:
                    STATE.ports[port]["connected"] = False
            time.sleep(1.5)
            if not os.path.exists(port):
                with STATE.lock:
                    STATE.ports.pop(port, None)
                return
            continue

        with STATE.lock:
            STATE.ports[port] = {"role": "?", "connected": True, "last": time.time()}

        role = "?"
        wlock = threading.Lock()
        buf = b""
        pending = None  # (nbytes, width, height, seq)

        def note_role(r):
            # Sticky. Identity comes from a handful of distinctive lines; every
            # other line ("[tx] acked", "[preview] on") arrives with r still "?"
            # and must not erase what we already know -- /api/camera routes
            # commands by role, so a downgrade silently drops them.
            with STATE.lock:
                known = STATE.ports.get(port, {}).get("role", "?")
                effective = r if r != "?" else known
                STATE.ports[port] = {
                    "role": effective, "connected": True, "last": time.time()
                }
                if effective == "camera":
                    STATE.serials["camera"] = (ser, wlock)

        try:
            while True:
                if time.time() < STATE.pause_until:
                    break          # release the port so an upload can proceed
                chunk = ser.read(8192)
                if chunk:
                    buf += chunk

                while True:
                    if pending is not None:
                        n, w, h, seq = pending
                        if len(buf) < n:
                            break
                        STATE.set_frame(buf[:n], w, h, seq)
                        buf = buf[n:]
                        pending = None
                        if role != "camera":
                            role = "camera"
                            note_role(role)
                        continue

                    if b"\n" not in buf:
                        break
                    raw, buf = buf.split(b"\n", 1)
                    line = raw.decode("utf-8", "replace").strip()
                    if not line:
                        continue

                    m = FRAME_RE.match(line)
                    if m:
                        pending = tuple(int(x) for x in m.groups())
                        continue

                    if "gateway node" in line:
                        role = "gateway"
                    elif "camera node" in line or "USB camera stream" in line:
                        role = "camera"
                    m = OCR_RE.match(line)
                    if m:
                        with STATE.lock:
                            STATE.ocr = {
                                "t": time.time(),
                                "value": int(m.group(1)), "ok": m.group(2) == "1",
                                "confidence": int(m.group(3)), "threshold": int(m.group(4)),
                                "digits_text": m.group(5),
                            }
                        role = "camera"
                        note_role(role)
                        continue
                    m = ROI_RE.match(line)
                    if m:
                        g = [int(x) if x is not None else 0 for x in m.groups()]
                        with STATE.lock:
                            STATE.diag = {
                                "min": g[0], "mean": g[1], "max": g[2],
                                "contrast": g[3], "threshold": g[4],
                                "trim_x": g[5], "trim_y": g[6],
                                "trim_w": g[7], "trim_h": g[8],
                                "border_ink": (g[9] if len(g) > 9 else 0),
                            }
                        role = "camera"; note_role(role)
                        continue
                    m = SEG_RE.match(line)
                    if m:
                        idx = int(m.group(1))
                        fills = {}
                        for pair in m.group(2).split():
                            k, v = pair.split("=")
                            fills[k] = int(v)
                        with STATE.lock:
                            STATE.segs[idx] = {"digit": idx, "fills": fills,
                                               "read": m.group(3)}
                        role = "camera"; note_role(role)
                        continue
                    m = CFG_RE.match(line)
                    if m:
                        g = [int(x) if x is not None else 0 for x in m.groups()]
                        with STATE.lock:
                            STATE.cfg = {
                                "roi_x": g[0], "roi_y": g[1], "roi_w": g[2], "roi_h": g[3],
                                "digits": g[4], "wake": g[5], "thr": g[6],
                                "invert": g[7], "lamp": g[8],
                                "bright": (g[9] if len(g) > 9 else 255),
                                "hold": (g[10] if len(g) > 10 else 0),
                                "hb": (g[11] if len(g) > 11 else None),
                                "sleep": (g[12] if len(g) > 12 else 0),
                                "flip": (g[13] if len(g) > 13 else 0),
                                "mirror": (g[14] if len(g) > 14 else 0),
                                "rotate": (g[15] if len(g) > 15 else 0),
                                "fine": (g[16] if len(g) > 16 else 0),
                                "ap": (g[17] if len(g) > 17 else 0),
                            }
                        role = "camera"
                        note_role(role)
                        continue
                    m = READING_RE.match(line)
                    if m:
                        with STATE.lock:
                            STATE.readings.append({
                                "t": time.time(), "seq": int(m.group(1)),
                                "value": int(m.group(2)), "digits": int(m.group(3)),
                                "confidence": int(m.group(4)), "rssi": float(m.group(5)),
                                "snr": float(m.group(6)), "vbat": int(m.group(7)),
                                "decode_ok": m.group(8) != "0",
                            })
                            if len(STATE.readings) > 500:
                                del STATE.readings[:-500]
                        role = "gateway"
                        note_role(role)
                        continue

                    if line.startswith("[radio]"):
                        with STATE.lock:
                            STATE.radio[role] = line
                    note_role(role)

                    # Identify CSV rows by shape, not by having caught the boot
                    # banner: the server is usually started long after the nodes
                    # booted, so the banner may never be seen at all.
                    # gateway: seq,rssi,snr,ferr,vbat,rx_ok,missed,pdr   (8)
                    # camera : seq,tx_ok,ack,up_r,up_s,dn_r,dn_s,pdr,vbat (9)
                    if ROW_RE.match(line):
                        fields = line.split(",")
                        if len(fields) == 8:
                            role = "gateway"
                            pkt = parse_gateway_row(fields)
                            if pkt["rssi"] is not None:
                                with STATE.lock:
                                    STATE.packets.append(pkt)
                                    if len(STATE.packets) > MAX_PACKETS:
                                        del STATE.packets[:-MAX_PACKETS]
                        elif len(fields) == 9:
                            role = "camera"
        except Exception:
            pass
        finally:
            try:
                ser.close()
            except Exception:
                pass
            with STATE.lock:
                if port in STATE.ports:
                    STATE.ports[port]["connected"] = False
                if STATE.serials.get("camera", (None, None))[0] is ser:
                    STATE.serials.pop("camera", None)
        time.sleep(1.0)


def port_watcher():
    """Pick up boards as they are plugged in, without restarting the server."""
    owned = set()
    while True:
        for port in sorted(glob.glob("/dev/cu.usbmodem*")):
            if port not in owned:
                owned.add(port)
                threading.Thread(target=reader, args=(port,), daemon=True).start()
        owned = {p for p in owned if os.path.exists(p)}
        time.sleep(2.0)


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass  # keep the console clean for the walk

    def _send(self, code, body, ctype="application/json", extra=None):
        if isinstance(body, str):
            body = body.encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        for k, v in (extra or {}).items():
            self.send_header(k, v)
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        if self.path.startswith("/api/state"):
            payload = STATE.snapshot()
            payload["camera"] = STATE.camera_status()
            self._send(200, json.dumps(payload))
        elif self.path.startswith("/frame.jpg"):
            with STATE.lock:
                f = STATE.frame
            if not f:
                self._send(503, "no frame yet", "text/plain")
            else:
                self._send(200, f["jpeg"], "image/jpeg")
        elif self.path.startswith("/stream.mjpg"):
            self.stream_mjpeg()
        elif self.path.startswith("/api/export"):
            out = io.StringIO()
            w = csv.writer(out)
            w.writerow(["iso_time", "seq", "rssi_dbm", "snr_db",
                        "freq_err_hz", "vbat_mv", "missed", "pdr_pct"])
            with STATE.lock:
                rows = list(STATE.packets)
                marks = list(STATE.markers)
            for p in rows:
                w.writerow([
                    time.strftime("%Y-%m-%dT%H:%M:%S", time.localtime(p["t"])),
                    p["seq"], p["rssi"], p["snr"], p["ferr"],
                    p["vbat"], p["missed"], p["pdr"],
                ])
            w.writerow([])
            w.writerow(["marker_time", "label", "at_seq", "rssi_dbm"])
            for m in marks:
                w.writerow([
                    time.strftime("%Y-%m-%dT%H:%M:%S", time.localtime(m["t"])),
                    m["label"], m.get("seq"), m.get("rssi"),
                ])
            stamp = time.strftime("%Y%m%d-%H%M%S")
            self._send(200, out.getvalue(), "text/csv", {
                "Content-Disposition": f'attachment; filename="walktest-{stamp}.csv"'
            })
        else:
            try:
                with open(os.path.join(HERE, "walktest_ui.html"), "rb") as fh:
                    self._send(200, fh.read(), "text/html; charset=utf-8")
            except FileNotFoundError:
                self._send(404, "walktest_ui.html not found", "text/plain")

    def stream_mjpeg(self):
        """multipart/x-mixed-replace, which <img> renders natively - no JS decoder."""
        self.send_response(200)
        self.send_header("Age", "0")
        self.send_header("Cache-Control", "no-cache, private")
        self.send_header("Pragma", "no-cache")
        self.send_header("Content-Type", "multipart/x-mixed-replace; boundary=frame")
        self.end_headers()
        last_seq = None
        idle_since = time.time()
        try:
            while True:
                with STATE.lock:
                    f = STATE.frame
                if f and f["seq"] != last_seq:
                    last_seq = f["seq"]
                    idle_since = time.time()
                    jpeg = f["jpeg"]
                    self.wfile.write(b"--frame\r\n")
                    self.wfile.write(b"Content-Type: image/jpeg\r\n")
                    self.wfile.write(b"Content-Length: %d\r\n\r\n" % len(jpeg))
                    self.wfile.write(jpeg)
                    self.wfile.write(b"\r\n")
                    self.wfile.flush()
                else:
                    # Nothing new. Give up eventually so a dead camera does not
                    # hold the connection (and its thread) open forever.
                    if time.time() - idle_since > 30:
                        break
                    time.sleep(0.02)
        except (BrokenPipeError, ConnectionResetError):
            pass  # viewer navigated away
        except Exception:
            pass

    def do_POST(self):
        if self.path.startswith("/api/mark"):
            n = int(self.headers.get("Content-Length", "0"))
            try:
                payload = json.loads(self.rfile.read(n) or b"{}")
            except ValueError:
                payload = {}
            label = str(payload.get("label", "")).strip()[:80] or "unlabelled"
            with STATE.lock:
                last = STATE.packets[-1] if STATE.packets else None
                STATE.markers.append({
                    "t": time.time(),
                    "label": label,
                    "seq": last["seq"] if last else None,
                    "rssi": last["rssi"] if last else None,
                })
            self._send(200, json.dumps({"ok": True}))
        elif self.path.startswith("/api/camera"):
            n = int(self.headers.get("Content-Length", "0"))
            try:
                payload = json.loads(self.rfile.read(n) or b"{}")
            except ValueError:
                payload = {}
            cmd = str(payload.get("cmd", "")).strip()[:64]
            ok = STATE.send(cmd) if cmd else False
            self._send(200, json.dumps({"ok": ok}))
        elif self.path.startswith("/api/pause"):
            n = int(self.headers.get("Content-Length", "0"))
            try:
                payload = json.loads(self.rfile.read(n) or b"{}")
            except ValueError:
                payload = {}
            secs = max(1, min(300, int(payload.get("seconds", 90))))
            with STATE.lock:
                STATE.pause_until = time.time() + secs
                STATE.serials.pop("camera", None)
            self._send(200, json.dumps({"ok": True, "paused_for": secs}))
        elif self.path.startswith("/api/resume"):
            with STATE.lock:
                STATE.pause_until = 0.0
            self._send(200, json.dumps({"ok": True}))
        elif self.path.startswith("/api/reset"):
            with STATE.lock:
                STATE.packets.clear()
                STATE.markers.clear()
                STATE.started = time.time()
            self._send(200, json.dumps({"ok": True}))
        else:
            self._send(404, json.dumps({"error": "not found"}))


def lan_ip():
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(("8.8.8.8", 80))
        ip = s.getsockname()[0]
        s.close()
        return ip
    except Exception:
        return "127.0.0.1"


if __name__ == "__main__":
    threading.Thread(target=port_watcher, daemon=True).start()
    srv = ThreadingHTTPServer(("0.0.0.0", PORT), Handler)
    print("LoRa walk test dashboard", flush=True)
    print(f"  this Mac : http://localhost:{PORT}", flush=True)
    print(f"  phone    : http://{lan_ip()}:{PORT}   (same WiFi)", flush=True)
    print("Ctrl-C to stop.", flush=True)
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        print("\nstopped")
