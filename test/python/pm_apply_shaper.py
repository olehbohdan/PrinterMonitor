#!/usr/bin/env python3
"""
pm_apply_shaper.py - Receive a PSD payload from PrinterMonitor over the
Home Assistant pipeline, run shaper search, call PM_SHAPER_APPLY via
Moonraker REST.

This is meant to live on the Home Assistant host (or any machine that can
reach Moonraker) and be invoked by a shell_command. The PSD payload comes
in as a JSON file path (HA writes the event data to /tmp first, then calls
this script with the path).

Usage:
    pm_apply_shaper.py PSD_JSON  [--moonraker URL]  [--max-smoothing F]

PSD_JSON schema:
    {
      "axis": "X" | "Y",
      "fs": <float Hz>,
      "freqs": [..],
      "psd":   [..]
    }

Exits 0 on success, prints the chosen shaper to stdout. HA can capture
that and call PM_SHAPER_APPLY itself (preferred over this script issuing
the macro call - keeps Moonraker creds in HA).

Use --moonraker to make the script issue the call directly. Default off.
"""
from __future__ import annotations

import argparse
import json
import os
import sys
import urllib.request

import numpy as np

# Allow importing the local shaper_calibrate module
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from shaper_calibrate import find_best_shaper  # noqa: E402


def call_moonraker(base_url: str, gcode: str) -> None:
    url = base_url.rstrip("/") + "/printer/gcode/script"
    req = urllib.request.Request(
        url,
        data=json.dumps({"script": gcode}).encode("utf-8"),
        headers={"Content-Type": "application/json"},
        method="POST",
    )
    with urllib.request.urlopen(req, timeout=10) as resp:  # noqa: S310
        resp.read()


def main() -> int:
    p = argparse.ArgumentParser()
    p.add_argument("psd_json")
    p.add_argument("--moonraker", default=None,
                   help="If set, POST PM_SHAPER_APPLY to this Moonraker URL")
    p.add_argument("--max-smoothing", type=float, default=0.10)
    p.add_argument("--damping", type=float, default=0.10)
    args = p.parse_args()

    with open(args.psd_json) as f:
        payload = json.load(f)
    axis = str(payload["axis"]).upper()
    psd = np.asarray(payload["psd"], dtype=np.float64)
    if "freqs" in payload:
        freqs = np.asarray(payload["freqs"], dtype=np.float64)
    else:
        # Derive bin frequencies from sample rate. PSD bins span 0..fs/2.
        fs = float(payload["fs"])
        n = psd.size
        freqs = np.linspace(0.0, fs / 2.0, n, dtype=np.float64)

    best = find_best_shaper(freqs, psd,
                            damping_ratio=args.damping,
                            max_smoothing=args.max_smoothing)
    if best is None:
        print(f"NO_SHAPER axis={axis}")
        if args.moonraker:
            try:
                call_moonraker(args.moonraker,
                               f"M118 PM_SHAPER_NO_RESULT AXIS={axis}")
            except Exception as e:
                print(f"WARN: M118 failed: {e}")
        return 1

    cmd = (f"PM_SHAPER_APPLY TYPE_{axis}={best['name']} "
           f"FREQ_{axis}={best['freq']:.9f}")
    found_msg = (f"PM_SHAPER_FOUND AXIS={axis} TYPE={best['name']} "
                 f"FREQ={best['freq']:.1f} "
                 f"RESIDUAL={best['vibrations']*100:.2f}pct "
                 f"MAX_ACCEL={best['max_accel']:.0f}")
    print(f"axis={axis} type={best['name']} freq={best['freq']:.9f} "
          f"residual={best['vibrations']*100:.2f}% "
          f"max_accel={best['max_accel']:.0f}")
    print(f"GCODE: {cmd}")

    if args.moonraker:
        # Announce the result in the Klipper console BEFORE applying so the
        # user can see it even if the apply fails / is rejected.
        try:
            call_moonraker(args.moonraker, f"M118 {found_msg}")
        except Exception as e:
            print(f"WARN: announcement M118 failed: {e}")
        call_moonraker(args.moonraker, cmd)
        print("APPLIED via Moonraker")
    return 0


if __name__ == "__main__":
    sys.exit(main())
