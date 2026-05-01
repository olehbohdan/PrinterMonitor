#!/usr/bin/env python3
"""
analyze_shaper.py - FFT / PSD analysis of an accelerometer capture,
                    prints Klipper [input_shaper] suggestions.

Usage:
    python3 analyze_shaper.py captures/shaper_x_20250101_120000.csv
    python3 analyze_shaper.py captures/shaper_x_*.csv --axis x --no-plot

Columns expected: t_us, ax_g, ay_g, az_g
Lines starting with '#' are ignored.

Method:
    - Parse CSV, drop the DC component per axis.
    - Estimate real sample rate from timestamps.
    - Compute Welch PSD for ax, ay, az.
    - For the axis the board was accelerated along, find top peaks in
      [PEAK_FMIN .. PEAK_FMAX] Hz -> Klipper shaper_freq_x/y.
    - Plot all three PSDs (optional).

Klipper's own calibrate_shaper.py does a more thorough shaper-type sweep;
this script is for a quick resonance readout.
"""

import argparse
import os
import sys

import numpy as np

PEAK_FMIN = 5.0     # Hz - below this is usually printer motion / DC drift
PEAK_FMAX = 120.0   # Hz - input shaper cares about the lowest few peaks


def load_csv(path):
    t_us, ax, ay, az = [], [], [], []
    meta = {}
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            if line.startswith("#"):
                # parse "# range_g=4  odr_hz=1600  scale_g_per_lsb=0.0001"
                for tok in line.lstrip("# ").split():
                    if "=" in tok:
                        k, v = tok.split("=", 1)
                        meta[k] = v
                continue
            if line.startswith("t_us"):
                continue
            parts = line.split(",")
            if len(parts) != 4:
                continue
            try:
                t_us.append(int(parts[0]))
                ax.append(float(parts[1]))
                ay.append(float(parts[2]))
                az.append(float(parts[3]))
            except ValueError:
                continue
    t = np.asarray(t_us, dtype=np.float64) * 1e-6
    return t, np.asarray(ax), np.asarray(ay), np.asarray(az), meta


def welch_psd(x, fs, nperseg=None):
    """Minimal Welch PSD (Hann window, 50% overlap) using only numpy."""
    x = np.asarray(x, dtype=np.float64)
    x = x - x.mean()
    n = len(x)
    if nperseg is None:
        nperseg = min(2048, n)
    if n < nperseg:
        nperseg = n
    step = max(nperseg // 2, 1)
    win = np.hanning(nperseg)
    win_norm = (win ** 2).sum() * fs

    segs = []
    for start in range(0, n - nperseg + 1, step):
        seg = x[start:start + nperseg] * win
        spec = np.fft.rfft(seg)
        psd = (np.abs(spec) ** 2) / win_norm
        segs.append(psd)
    if not segs:
        return np.array([0.0]), np.array([0.0])
    psd = np.mean(segs, axis=0)
    # One-sided: double non-DC/Nyquist bins
    psd[1:-1] *= 2
    freqs = np.fft.rfftfreq(nperseg, d=1.0 / fs)
    return freqs, psd


def find_peaks(freqs, psd, fmin=PEAK_FMIN, fmax=PEAK_FMAX, n=3, min_sep_hz=3.0):
    mask = (freqs >= fmin) & (freqs <= fmax)
    f = freqs[mask]
    p = psd[mask]
    if len(p) < 3:
        return []
    # local maxima
    is_peak = (p[1:-1] > p[:-2]) & (p[1:-1] > p[2:])
    idx = np.where(is_peak)[0] + 1
    peaks = sorted(((f[i], p[i]) for i in idx), key=lambda x: -x[1])

    # Enforce minimum spacing
    chosen = []
    for fp, pp in peaks:
        if all(abs(fp - fc) >= min_sep_hz for fc, _ in chosen):
            chosen.append((fp, pp))
        if len(chosen) >= n:
            break
    return chosen


def analyze_axis(label, x, fs):
    freqs, psd = welch_psd(x, fs)
    peaks = find_peaks(freqs, psd)
    return freqs, psd, peaks


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("csv", nargs="+")
    ap.add_argument("--axis", choices=["x", "y", "auto"], default="auto",
                    help="Which axis the board was moved along (for Klipper suggestion)")
    ap.add_argument("--no-plot", action="store_true")
    args = ap.parse_args()

    csvs = []
    for pat in args.csv:
        if any(ch in pat for ch in "*?["):
            import glob
            csvs += sorted(glob.glob(pat))
        else:
            csvs.append(pat)

    for path in csvs:
        print(f"\n=== {path} ===")
        t, ax, ay, az, meta = load_csv(path)
        if len(t) < 100:
            print("  (too few samples)")
            continue

        dur = t[-1] - t[0]
        fs = (len(t) - 1) / dur
        print(f"  samples = {len(t)}   duration = {dur:.2f} s   fs = {fs:.1f} Hz")
        if meta:
            print(f"  meta    = {meta}")

        f_x, p_x, pk_x = analyze_axis("X", ax, fs)
        f_y, p_y, pk_y = analyze_axis("Y", ay, fs)
        f_z, p_z, pk_z = analyze_axis("Z", az, fs)

        for lbl, peaks in (("X", pk_x), ("Y", pk_y), ("Z", pk_z)):
            if not peaks:
                print(f"  {lbl}: no peaks in {PEAK_FMIN}-{PEAK_FMAX} Hz")
            else:
                ps = ", ".join(f"{f:.1f} Hz (p={p:.2e})" for f, p in peaks)
                print(f"  {lbl}: {ps}")

        # Pick axis for Klipper suggestion
        if args.axis == "auto":
            # pick the axis with the largest total in-band energy
            mask = (f_x >= PEAK_FMIN) & (f_x <= PEAK_FMAX)
            energies = {"x": p_x[mask].sum(), "y": p_y[mask].sum(), "z": p_z[mask].sum()}
            axis = max(energies, key=energies.get)
        else:
            axis = args.axis
        peaks_for_axis = {"x": pk_x, "y": pk_y, "z": pk_z}[axis]
        if peaks_for_axis:
            f0 = peaks_for_axis[0][0]
            print(f"\n  Klipper suggestion (axis={axis}):")
            print(f"    [input_shaper]")
            print(f"    shaper_freq_{axis} = {f0:.1f}")
            print(f"    shaper_type_{axis} = mzv    # try ei / 2hump_ei if residual ringing")

        if not args.no_plot:
            try:
                import matplotlib.pyplot as plt
            except ImportError:
                print("  (matplotlib not installed; skipping plot. pip install matplotlib)")
                continue
            fig, ax_ = plt.subplots(figsize=(9, 5))
            ax_.semilogy(f_x, p_x + 1e-20, label="X")
            ax_.semilogy(f_y, p_y + 1e-20, label="Y")
            ax_.semilogy(f_z, p_z + 1e-20, label="Z")
            ax_.set_xlim(0, max(PEAK_FMAX + 20, 100))
            ax_.set_xlabel("Frequency (Hz)")
            ax_.set_ylabel("PSD (g²/Hz)")
            ax_.set_title(os.path.basename(path))
            ax_.grid(True, which="both", alpha=0.3)
            ax_.legend()
            for fp, _ in peaks_for_axis[:3]:
                ax_.axvline(fp, color="red", alpha=0.3, linestyle="--")
            png = os.path.splitext(path)[0] + ".png"
            fig.tight_layout()
            fig.savefig(png, dpi=120)
            print(f"  plot saved -> {png}")
            plt.show()


if __name__ == "__main__":
    main()
