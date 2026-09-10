#!/usr/bin/env python3
"""Map a board's USB serial number to its current /dev path.

macOS renumbers /dev/cu.usbmodem* by physical port, so a board that moves to a
different socket takes on a different name -- and can take on the name its
neighbour used to have. Flashing by remembered path is therefore unsafe: the
gateway and the Sense have swapped names once already in this project.

    python3 tools/find_port.py sense      -> /dev/cu.usbmodemXXXX
    python3 tools/find_port.py gateway
    python3 tools/find_port.py --list
"""
import subprocess
import sys

BOARDS = {
    "gateway": "68:EE:8F:4C:2C:18",   # plain XIAO ESP32S3 + B2B Wio-SX1262
    "sense":   "68:EE:8F:47:0B:50",   # XIAO ESP32S3 Sense + pin-header module
}


def ports():
    """[(device, serial)] for every attached USB serial device."""
    out = subprocess.run(["pio", "device", "list"], capture_output=True, text=True).stdout
    found, dev = [], None
    for line in out.splitlines():
        line = line.strip()
        if line.startswith("/dev/"):
            dev = line
        elif line.startswith("Hardware ID:") and dev:
            ser = ""
            for part in line.split():
                if part.startswith("SER="):
                    ser = part[4:]
            found.append((dev, ser))
            dev = None
    return found


def main():
    if len(sys.argv) < 2 or sys.argv[1] in ("--list", "-l"):
        for dev, ser in ports():
            name = next((n for n, s in BOARDS.items() if s == ser), "unknown")
            print(f"{dev:<28} {ser or '(none)':<20} {name}")
        return 0

    want = sys.argv[1].lower()
    if want not in BOARDS:
        print(f"unknown board '{want}'; known: {', '.join(BOARDS)}", file=sys.stderr)
        return 2
    for dev, ser in ports():
        if ser == BOARDS[want]:
            print(dev)
            return 0
    print(f"{want} not attached", file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main())
