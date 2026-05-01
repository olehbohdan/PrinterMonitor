#!/usr/bin/env python3
"""
capture.py  - Read BMI270 CSV stream from the ESP32-C3 board and save it.

Usage:
    python3 capture.py                           # auto-find port, save to ./captures/
    python3 capture.py --port /dev/cu.usbmodem1101
    python3 capture.py --out my_run.csv

Workflow:
    1. Flash the `shaper` env:   pio run -e shaper -t upload
    2. Mount the board rigidly to the printer toolhead (X or Y carriage).
    3. Start this script. It prints the header then waits.
    4. On the printer, start a ringing move (fast back-and-forth G-code).
    5. Flip SW3 HIGH to start capture, LOW to stop.
    6. Script saves the CSV and exits (or keep running for more runs with --loop).
"""

import argparse
import datetime as dt
import glob
import os
import sys
import time

import serial  # pip install pyserial


def find_port():
    for pattern in ("/dev/cu.usbmodem*", "/dev/ttyACM*", "/dev/ttyUSB*"):
        matches = sorted(glob.glob(pattern))
        if matches:
            return matches[0]
    return None


def capture(port: str, out_path: str) -> str:
    os.makedirs(os.path.dirname(out_path) or ".", exist_ok=True)
    ser = serial.Serial(port, 115200, timeout=1)
    print(f"[capture] port={port}  out={out_path}")
    print("[capture] waiting for '# START' (flip SW3 HIGH on the board)...")

    with open(out_path, "w") as f:
        started = False
        n = 0
        t0 = None
        while True:
            line = ser.readline().decode(errors="replace").strip()
            if not line:
                continue

            if line.startswith("# ERROR"):
                print(line, file=sys.stderr)
                return ""

            if not started:
                # Echo metadata (header lines) and the CSV column line.
                if line.startswith("#") or line.startswith("t_us"):
                    print(line)
                    f.write(line + "\n")
                if line == "# START":
                    started = True
                    t0 = time.time()
                    print("[capture] recording...")
                continue

            if line.startswith("# STOP"):
                f.write(line + "\n")
                dt_s = time.time() - t0
                print(f"[capture] done. {n} samples in {dt_s:.2f}s ({n/max(dt_s,1e-6):.0f} Hz wall)")
                print(line)
                return out_path

            f.write(line + "\n")
            n += 1
            if n % 1000 == 0:
                print(f"[capture] {n} samples...", end="\r")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default=None)
    ap.add_argument("--out", default=None)
    ap.add_argument("--axis", default="xy", help="label tag saved in filename (e.g. 'x', 'y')")
    ap.add_argument("--analyze", action="store_true",
                    help="Run analyze_shaper.py on the file after capture")
    args = ap.parse_args()

    port = args.port or find_port()
    if not port:
        print("No serial port found. Pass --port /dev/cu.usbmodemXXXX", file=sys.stderr)
        sys.exit(1)

    if args.out:
        out = args.out
    else:
        ts = dt.datetime.now().strftime("%Y%m%d_%H%M%S")
        out = f"captures/shaper_{args.axis}_{ts}.csv"

    path = capture(port, out)
    if not path:
        sys.exit(1)

    if args.analyze:
        here = os.path.dirname(os.path.abspath(__file__))
        os.system(f"python3 {here}/analyze_shaper.py {path}")


if __name__ == "__main__":
    main()
