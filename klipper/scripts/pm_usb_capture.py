#!/usr/bin/env python3
"""
PrinterMonitor USB shaper capture.

Opens the PrinterMonitor board over USB-Serial-JTAG, streams raw BMI270
samples at 1.6 kHz, and writes a Klipper-compatible resonance CSV
(time,accel_x,accel_y,accel_z in m/s^2) ready for
`~/klipper/scripts/calibrate_shaper.py`.

Usage:
    pm_usb_capture.py --axis X --duration 3.0 --out /tmp/resonances_X.csv
    pm_usb_capture.py --axis X --duration 3.0 --out /tmp/r.csv \\
        --calibrate ~/klipper/scripts/calibrate_shaper.py

Frame format on the wire:
    0xAA 0x55  ax_lo ax_hi  ay_lo ay_hi  az_lo az_hi   (8 bytes)
    int16 little-endian, raw LSBs at +-8 g.

Defaults assume the device enumerates as a single ttyACM* node.  Pass
--port to override.
"""
from __future__ import annotations

import argparse
import glob
import os
import struct
import subprocess
import sys
import time
from typing import Iterable, List, Optional

import serial  # pyserial


SYNC0 = 0xAA
SYNC1 = 0x55
FRAME_LEN = 8
LSB_PER_G = 32768.0 / 8.0  # +-8 g range
G_TO_MS2 = 9.80665


def find_port() -> str:
    """Pick the first ttyACM*/ttyUSB* candidate."""
    for pattern in (
        "/dev/serial/by-id/usb-Espressif_*",
        "/dev/serial/by-id/usb-*PrinterMonitor*",
        "/dev/ttyACM*",
        "/dev/ttyUSB*",
    ):
        hits = sorted(glob.glob(pattern))
        if hits:
            return hits[0]
    raise SystemExit("no USB serial device found - is the board plugged in?")


def parse_frames(buf: bytearray) -> Iterable[tuple]:
    """Yield (ax_lsb, ay_lsb, az_lsb) tuples; consume bytes from buf."""
    i = 0
    while i + FRAME_LEN <= len(buf):
        if buf[i] == SYNC0 and buf[i + 1] == SYNC1:
            ax, ay, az = struct.unpack_from("<hhh", buf, i + 2)
            yield ax, ay, az
            i += FRAME_LEN
        else:
            # Resync: drop one byte and retry.
            i += 1
    # Trim consumed bytes.
    del buf[:i]


def capture(port: str, duration_s: float, axis: str,
            settle_s: float = 0.1) -> List[tuple]:
    """Open port, send START, collect samples for `duration_s`, send STOP.

    Returns a list of (t_sec, ax_ms2, ay_ms2, az_ms2).
    """
    ser = serial.Serial(port, baudrate=115200, timeout=0.05)
    # ESP32-C3 USB-Serial-JTAG ignores baud, but pyserial wants something.

    # Drain any leftover bytes from a previous session.
    time.sleep(0.05)
    ser.reset_input_buffer()

    # Probe.
    ser.write(b"PING\n")
    ser.flush()
    deadline = time.monotonic() + 1.0
    pong = b""
    while time.monotonic() < deadline:
        chunk = ser.read(64)
        pong += chunk
        if b"PONG" in pong:
            break
    if b"PONG" not in pong:
        raise SystemExit(
            f"no PONG from {port} - is the firmware loaded? got={pong!r}"
        )

    # Start streaming.
    ser.reset_input_buffer()
    ser.write(b"START\n")
    ser.flush()

    # Wait for "OK\n" so we know the chip switched ODR before t0.
    ack = b""
    deadline = time.monotonic() + 1.0
    while time.monotonic() < deadline and b"OK" not in ack:
        ack += ser.read(64)
    if b"OK" not in ack:
        raise SystemExit(f"no OK from board on START, got={ack!r}")

    # The OK byte stream may have leaked into the binary frame buffer.
    # Reset and let the chip resynchronize on SYNC bytes.
    time.sleep(settle_s)
    ser.reset_input_buffer()

    raw = bytearray()
    samples: List[tuple] = []
    t0 = time.monotonic()
    end_t = t0 + duration_s

    while time.monotonic() < end_t:
        chunk = ser.read(2048)
        if chunk:
            raw.extend(chunk)
            for ax, ay, az in parse_frames(raw):
                # Timestamp by sample index (ODR is exact 1600 Hz inside
                # the chip; software wall-clock has too much jitter).
                # We assign the proper time after we know how many we got.
                samples.append((ax, ay, az))

    wall_elapsed = time.monotonic() - t0
    fs_actual_wall = len(samples) / wall_elapsed if wall_elapsed > 0 else 0.0

    # Stop.
    ser.write(b"STOP\n")
    ser.flush()
    time.sleep(0.05)
    ser.close()

    if len(samples) < 256:
        raise SystemExit(
            f"only got {len(samples)} samples - chirp probably didn't run"
        )

    # Emit timestamps based on actual wall-clock fs (more honest than
    # blindly trusting the chip nominal 1600 Hz).  If wall fs is in a
    # plausible range [1200, 1700] Hz, use it; otherwise fall back to
    # 1600 (the chip-side ODR).
    fs_use = fs_actual_wall if 1200.0 <= fs_actual_wall <= 1700.0 else 1600.0
    dt = 1.0 / fs_use
    out: List[tuple] = []
    for i, (ax, ay, az) in enumerate(samples):
        t = i * dt
        out.append((
            t,
            ax / LSB_PER_G * G_TO_MS2,
            ay / LSB_PER_G * G_TO_MS2,
            az / LSB_PER_G * G_TO_MS2,
        ))
    # Stash the measured fs on the list for the caller to print.
    out.append(("__fs__", fs_actual_wall, fs_use, len(samples)))
    return out


def write_csv(path: str, samples: List[tuple]) -> None:
    """Write Klipper-compatible resonance CSV."""
    with open(path, "w") as f:
        f.write("#time,accel_x,accel_y,accel_z\n")
        for t, ax, ay, az in samples:
            f.write(f"{t:.6f},{ax:.6f},{ay:.6f},{az:.6f}\n")


def run_calibrate(calibrate_path: str, csv_path: str,
                  axis: Optional[str] = None) -> None:
    """Invoke Klipper's calibrate_shaper.py on the CSV, mirror output to stdout.

    Klipper's calibrate_shaper.py needs numpy + matplotlib which on a
    typical Klipper Pi only live in /usr/bin/python3, NOT in
    ~/klippy-env (that venv is intentionally minimal).  Use system
    python explicitly.
    """
    cmd = ["/usr/bin/python3", calibrate_path, csv_path]
    print(f"[pm] running: {' '.join(cmd)}", flush=True)
    proc = subprocess.run(cmd, check=False)
    if proc.returncode != 0:
        raise SystemExit(f"calibrate_shaper.py exit {proc.returncode}")


def main() -> int:
    p = argparse.ArgumentParser()
    p.add_argument("--port", default=None,
                   help="serial port (auto-detected if omitted)")
    p.add_argument("--axis", default="X", choices=["X", "Y", "x", "y"],
                   help="axis label for CSV / shaper script")
    p.add_argument("--duration", type=float, default=2.5,
                   help="seconds to capture (default 2.5)")
    p.add_argument("--out", default="/tmp/pm_resonances.csv",
                   help="output CSV path")
    p.add_argument("--calibrate", default=None,
                   help="path to klipper/scripts/calibrate_shaper.py "
                        "(if set, run it on the CSV after capture)")
    p.add_argument("--no-output", action="store_true",
                   help="suppress 'PM_*' lines on stdout (used when invoked "
                        "from gcode_shell_command which echos to console)")
    args = p.parse_args()

    port = args.port or find_port()
    if not args.no_output:
        print(f"[pm] using port: {port}", flush=True)

    samples = capture(port, args.duration, args.axis.upper())
    # capture() appends a synthetic ("__fs__", fs_wall, fs_used, n) tail.
    meta = samples.pop()
    _, fs_wall, fs_used, n_samples = meta
    if not args.no_output:
        print(f"[pm] captured {n_samples} samples, "
              f"wall-clock fs={fs_wall:.1f} Hz "
              f"(CSV uses fs={fs_used:.0f} Hz)", flush=True)

    write_csv(args.out, samples)
    if not args.no_output:
        print(f"[pm] wrote {args.out}", flush=True)
        print(f"PM_USB_CAPTURE AXIS={args.axis.upper()} "
              f"SAMPLES={n_samples} FS={fs_wall:.0f} CSV={args.out}",
              flush=True)

    if args.calibrate:
        if not os.path.isfile(args.calibrate):
            raise SystemExit(f"calibrate_shaper.py not found at {args.calibrate}")
        run_calibrate(args.calibrate, args.out, axis=args.axis)

    return 0


if __name__ == "__main__":
    sys.exit(main())
