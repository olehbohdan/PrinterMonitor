#!/usr/bin/env python3
"""
shaper_calibrate.py - Standalone port of Klipper's input-shaper search.

Originally derived from klipper/scripts/shaper_calibrate.py (GPLv3, Kevin
O'Connor & contributors). Trimmed to the bits we need on the host side of
the PrinterMonitor pipeline:

    PSD (freq, power) for one axis -> (shaper_type, shaper_freq, residual,
                                       smoothing, max_accel)

Inputs:
    freqs  : 1D ndarray, Hz
    psd    : 1D ndarray, same length, power-spectral-density of detrended
             accel along the axis under test (in g**2 / Hz; absolute units
             don't matter, only relative shape)

Output (best result among candidate types) printed and returned as a dict:
    {
      "type":      "mzv",
      "freq":      43.2,
      "vibrations": 0.18,
      "smoothing": 0.07,
      "max_accel": 6843.0,
    }

This module is import-safe (no side effects). Run as a script with a CSV
to test:
    python3 shaper_calibrate.py captures/shaper_x.csv x
"""

from __future__ import annotations

import math
import sys
from collections import namedtuple

import numpy as np

# ---------------------------------------------------------------------------
# Shaper definitions (verbatim from Klipper's shaper_calibrate.py).
# Each shaper takes (shaper_freq_hz, damping_ratio) and returns
# (amplitudes, time_offsets) - convolution with delta-impulses gives the
# transfer function H(f).
# ---------------------------------------------------------------------------

def get_zv_shaper(shaper_freq, damping_ratio):
    df = math.sqrt(1.0 - damping_ratio**2)
    K = math.exp(-damping_ratio * math.pi / df)
    t_d = 1.0 / (shaper_freq * df)
    A = [1.0, K]
    T = [0.0, 0.5 * t_d]
    return (A, T)

def get_zvd_shaper(shaper_freq, damping_ratio):
    df = math.sqrt(1.0 - damping_ratio**2)
    K = math.exp(-damping_ratio * math.pi / df)
    t_d = 1.0 / (shaper_freq * df)
    A = [1.0, 2.0 * K, K * K]
    T = [0.0, 0.5 * t_d, t_d]
    return (A, T)

def get_mzv_shaper(shaper_freq, damping_ratio):
    df = math.sqrt(1.0 - damping_ratio**2)
    K = math.exp(-0.75 * damping_ratio * math.pi / df)
    t_d = 1.0 / (shaper_freq * df)
    a1 = 1.0 - 1.0 / math.sqrt(2.0)
    a2 = (math.sqrt(2.0) - 1.0) * K
    a3 = a1 * K * K
    A = [a1, a2, a3]
    T = [0.0, 0.375 * t_d, 0.75 * t_d]
    return (A, T)

def get_ei_shaper(shaper_freq, damping_ratio):
    v_tol = 1.0 / 100.0
    df = math.sqrt(1.0 - damping_ratio**2)
    K = math.exp(-damping_ratio * math.pi / df)
    t_d = 1.0 / (shaper_freq * df)
    a1 = 0.25 * (1.0 + v_tol)
    a2 = 0.5 * (1.0 - v_tol) * K
    a3 = a1 * K * K
    A = [a1, a2, a3]
    T = [0.0, 0.5 * t_d, t_d]
    return (A, T)

def get_2hump_ei_shaper(shaper_freq, damping_ratio):
    v_tol = 1.0 / 100.0
    df = math.sqrt(1.0 - damping_ratio**2)
    K = math.exp(-damping_ratio * math.pi / df)
    t_d = 1.0 / (shaper_freq * df)
    V2 = v_tol**2
    X = pow(V2 * (math.sqrt(1.0 - V2) + 1.0), 1.0/3.0)
    a1 = (3.0 * X * X + 2.0 * X + 3.0 * V2) / (16.0 * X)
    a2 = (0.5 - a1) * K
    a3 = a2 * K
    a4 = a1 * K * K * K
    A = [a1, a2, a3, a4]
    T = [0.0, 0.5 * t_d, t_d, 1.5 * t_d]
    return (A, T)

def get_3hump_ei_shaper(shaper_freq, damping_ratio):
    v_tol = 1.0 / 100.0
    df = math.sqrt(1.0 - damping_ratio**2)
    K = math.exp(-damping_ratio * math.pi / df)
    t_d = 1.0 / (shaper_freq * df)
    K2 = K * K
    a1 = 0.0625 * (1.0 + 3.0 * v_tol + 2.0 * math.sqrt(2.0 * (v_tol + 1.0) * v_tol))
    a2 = 0.25 * (1.0 - v_tol) * K
    a3 = (0.5 * (1.0 + v_tol) - 2.0 * a1) * K2
    a4 = a2 * K2
    a5 = a1 * K2 * K2
    A = [a1, a2, a3, a4, a5]
    T = [0.0, 0.5 * t_d, t_d, 1.5 * t_d, 2.0 * t_d]
    return (A, T)


InputShaperCfg = namedtuple("InputShaperCfg",
                            ("name", "init_func", "min_freq"))

INPUT_SHAPERS = [
    InputShaperCfg("zv",       get_zv_shaper,       21.0),
    InputShaperCfg("mzv",      get_mzv_shaper,      23.0),
    InputShaperCfg("zvd",      get_zvd_shaper,      29.0),
    InputShaperCfg("ei",       get_ei_shaper,       29.0),
    InputShaperCfg("2hump_ei", get_2hump_ei_shaper, 39.0),
    InputShaperCfg("3hump_ei", get_3hump_ei_shaper, 48.0),
]

# Klipper defaults
DEFAULT_DAMPING_RATIO = 0.1
TARGET_VIB           = 0.005   # 0.5% residual
DEFAULT_MAX_SMOOTHING = 0.10


# ---------------------------------------------------------------------------
# Core scoring (vectorised, vendored from Klipper's CalibrationData /
# ShaperCalibrate)
# ---------------------------------------------------------------------------

def _shaper_response(A, T, freqs, damping_ratio):
    """|H(f)| of the shaper across `freqs`."""
    A = np.asarray(A, dtype=np.float64)
    T = np.asarray(T, dtype=np.float64)
    inv_D = 1.0 / A.sum()
    omega = 2.0 * math.pi * freqs
    damping = damping_ratio * omega
    omega_d = omega * math.sqrt(1.0 - damping_ratio**2)
    # outer products
    W = A[None, :] * np.exp(-damping[:, None] * (T[None, :] - T[-1]))
    S = (W * np.sin(omega_d[:, None] * T[None, :])).sum(axis=1)
    C = (W * np.cos(omega_d[:, None] * T[None, :])).sum(axis=1)
    return np.sqrt(S * S + C * C) * inv_D


def _shaper_smoothing(A, T, accel=5000.0, scv=5.0):
    half_accel = 0.5 * accel
    A = np.asarray(A, dtype=np.float64)
    T = np.asarray(T, dtype=np.float64)
    inv_D = 1.0 / A.sum()
    ts = (A * T).sum() * inv_D
    offsets = T - ts
    # Y0 from Klipper - average sq displacement during a constant accel move
    return half_accel * ((A * offsets * offsets).sum() * inv_D
                         - ((A * offsets).sum() * inv_D) ** 2) / (scv * scv)


def _max_accel_for_smoothing(A, T, max_smoothing):
    sm_unit = _shaper_smoothing(A, T, accel=1.0)
    if sm_unit <= 0:
        return 1e9
    return max_smoothing / sm_unit


def fit_shaper(name, init_func, min_freq, freqs, psd, damping_ratio,
               max_smoothing, scv):
    """Find best shaper_freq for one shaper type."""
    test_freqs = np.arange(min_freq, 200.0, 0.2)
    best = None
    # measured PSD must be on the same freq grid as eval; interpolate.
    # Klipper integrates vibration as sum(psd * H^2) - we mirror that.
    psd_total = max(np.trapz(psd, freqs), 1e-30)
    for sf in test_freqs:
        A, T = init_func(sf, damping_ratio)
        H = _shaper_response(A, T, freqs, damping_ratio)
        vib = np.trapz(psd * H * H, freqs) / psd_total
        sm = _shaper_smoothing(A, T, accel=5000.0, scv=scv)
        max_accel = _max_accel_for_smoothing(A, T, max_smoothing)
        if sm > max_smoothing:
            continue
        # Klipper's score = vib + sm * 0.5 (smoothness penalty)
        score = vib * (sm + 0.5)
        if best is None or score < best["score"]:
            best = dict(name=name, freq=float(sf), vibrations=float(vib),
                        smoothing=float(sm), max_accel=float(max_accel),
                        score=float(score))
    return best


def find_best_shaper(freqs, psd,
                     damping_ratio=DEFAULT_DAMPING_RATIO,
                     max_smoothing=DEFAULT_MAX_SMOOTHING,
                     scv=5.0,
                     verbose=False):
    """Run all shaper types and return the lowest-score result."""
    results = []
    for cfg in INPUT_SHAPERS:
        r = fit_shaper(cfg.name, cfg.init_func, cfg.min_freq,
                       freqs, psd, damping_ratio, max_smoothing, scv)
        if r is not None:
            results.append(r)
            if verbose:
                print(f"  {r['name']:<10}  f={r['freq']:5.1f} Hz  "
                      f"vib={r['vibrations']*100:5.2f}%  "
                      f"sm={r['smoothing']:.3f}  "
                      f"max_accel={r['max_accel']:6.0f}  "
                      f"score={r['score']:.4f}")
    if not results:
        return None
    results.sort(key=lambda r: r["score"])
    return results[0]


# ---------------------------------------------------------------------------
# CLI helper - reuses analyze_shaper.py's CSV loader
# ---------------------------------------------------------------------------

def _main():
    if len(sys.argv) < 2:
        print("usage: shaper_calibrate.py CSV [axis=x|y|z]")
        sys.exit(2)
    csv_path = sys.argv[1]
    axis = sys.argv[2].lower() if len(sys.argv) > 2 else "x"
    from analyze_shaper import load_csv, welch_psd  # type: ignore
    t, ax, ay, az, meta = load_csv(csv_path)
    if len(t) < 64:
        print("too few samples"); sys.exit(1)
    fs = (len(t) - 1) / max(t[-1] - t[0], 1e-6)
    sig = {"x": ax, "y": ay, "z": az}[axis]
    f, p = welch_psd(sig, fs)
    print(f"# fs={fs:.1f} Hz  bins={len(f)}  axis={axis}")
    best = find_best_shaper(f, p, verbose=True)
    print()
    if best:
        print(f"BEST: SHAPER_TYPE_{axis.upper()}={best['name']} "
              f"SHAPER_FREQ_{axis.upper()}={best['freq']:.1f}  "
              f"(residual={best['vibrations']*100:.2f}%, "
              f"max_accel={best['max_accel']:.0f})")


if __name__ == "__main__":
    _main()
