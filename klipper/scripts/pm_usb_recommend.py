#!/usr/bin/env python3
"""High-precision shaper picker for PrinterMonitor USB tuning.

Uses Klipper's shaper_calibrate library directly (instead of shelling out
to calibrate_shaper.py) so we can:

  1. Re-fit the chosen shaper at a fine 0.05 Hz frequency grid
     (calibrate_shaper.py uses 0.2 Hz steps -> only 5 picks per Hz).
  2. Apply parabolic interpolation on the raw PSD peak for sub-bin
     precision (typically 0.01-0.02 Hz on a 22 s capture).
  3. Filter out band-edge artifacts and over-smoothed shapers, then
     score survivors using vibrations + smoothing + distance-from-peak.

Usage:
    pm_usb_recommend.py AXIS FREQ_START FREQ_END [CSV]

Emits, e.g.:
    PM_USB_RESULT AXIS=X shaper_type_x=ei shaper_freq_x=53.85 vib=3.78% smooth=0.113 peak=48.24Hz Q=117.9
"""
import os
import sys

import numpy as np

# Tuning constants -----------------------------------------------------------
EDGE_MARGIN_HZ = 2.0
MAX_SMOOTHING = float(os.environ.get("PM_MAX_SMOOTHING", "0.20"))
SMOOTHING_WEIGHT = 100.0
PEAK_DISTANCE_WEIGHT = 0.5
FINE_FREQ_STEP = 0.05  # Hz, for re-fitting picked shaper

# Wire up Klipper's shaper_calibrate library --------------------------------
KLIPPER_PATH = os.path.expanduser("~/klipper")
sys.path.insert(0, os.path.join(KLIPPER_PATH, "klippy"))
import importlib  # noqa: E402
shaper_calibrate = importlib.import_module(".shaper_calibrate", "extras")
shaper_defs = importlib.import_module(".shaper_defs", "extras")


def parabolic_peak(p, i):
    """Sub-bin parabolic interpolation around index i in array p.

    Returns a fractional offset in [-1, 1] to add to i.  Standard 3-point
    formula: offset = 0.5 * (p[i-1] - p[i+1]) / (p[i-1] - 2*p[i] + p[i+1]).
    """
    if i <= 0 or i >= len(p) - 1:
        return 0.0
    a, b, c = float(p[i - 1]), float(p[i]), float(p[i + 1])
    denom = a - 2.0 * b + c
    if denom == 0:
        return 0.0
    return 0.5 * (a - c) / denom


def measure_psd_peak(csv_path, axis, fs_lo, fs_hi):
    """Return (peak_freq_hz, q_factor) for the dominant resonance.

    Uses a single full-length Hann FFT (chirps don't benefit from Welch
    averaging) plus parabolic interpolation for sub-bin precision.
    """
    try:
        col_idx = {"x": 1, "y": 2, "z": 3}[axis.lower()]
        data = np.loadtxt(csv_path, delimiter=",", skiprows=1)
        t = data[:, 0]
        a = data[:, col_idx]
        dt = float(np.median(np.diff(t)))
        if dt <= 0:
            return None, None
        a = a - np.mean(a)
        n = len(a)
        if n < 1024:
            return None, None
        win = np.hanning(n)
        spec = np.fft.rfft(a * win)
        psd = spec.real ** 2 + spec.imag ** 2
        freqs = np.fft.rfftfreq(n, dt)
        bin_hz = 1.0 / (n * dt)

        lo, hi = max(fs_lo - 1.0, 1.0), fs_hi + 1.0
        mask = (freqs >= lo) & (freqs <= hi)
        if not np.any(mask):
            return None, None
        f_in = freqs[mask]
        p_in = psd[mask]
        peak_i = int(np.argmax(p_in))
        # parabolic refinement
        offset = parabolic_peak(p_in, peak_i)
        peak_f = float(f_in[peak_i] + offset * bin_hz)
        peak_p = float(p_in[peak_i])

        # FWHM (linear interp on each side)
        half = peak_p * 0.5
        li = peak_i
        while li > 0 and p_in[li] > half:
            li -= 1
        ri = peak_i
        while ri < len(p_in) - 1 and p_in[ri] > half:
            ri += 1
        # linear interpolate to find exact half-power frequencies
        def interp_half(j_lo, j_hi):
            if j_lo == j_hi:
                return float(f_in[j_lo])
            y0, y1 = float(p_in[j_lo]), float(p_in[j_hi])
            x0, x1 = float(f_in[j_lo]), float(f_in[j_hi])
            if y1 == y0:
                return 0.5 * (x0 + x1)
            return x0 + (half - y0) * (x1 - x0) / (y1 - y0)

        f_left = interp_half(li, min(li + 1, len(p_in) - 1))
        f_right = interp_half(max(ri - 1, 0), ri)
        fwhm = max(f_right - f_left, 1e-9)
        q = peak_f / fwhm
        return peak_f, q
    except Exception as exc:
        print(f"// pm_usb_recommend: PSD peak detection failed: {exc}",
              file=sys.stderr)
        return None, None


def load_calibration(csv_path):
    """Load CSV and produce a CalibrationData object via Klipper's helper."""
    data = np.loadtxt(csv_path, comments="#", delimiter=",")
    helper = shaper_calibrate.ShaperCalibrate(printer=None)
    cd = helper.process_accelerometer_data(csv_path, data)
    cd.normalize_to_frequencies()
    return helper, cd


def main():
    if len(sys.argv) < 4:
        print("usage: pm_usb_recommend.py AXIS FREQ_START FREQ_END [CSV]",
              file=sys.stderr)
        sys.exit(2)
    axis = sys.argv[1].lower()
    fs = float(sys.argv[2])
    fe = float(sys.argv[3])
    csv = sys.argv[4] if len(sys.argv) > 4 else f"/tmp/pm_resonances_{axis}.csv"

    helper, cd = load_calibration(csv)

    # 1. Coarse fit at default 0.2 Hz step (parses every shaper).
    rows = []  # (name, freq, vib_pct, smoothing, shaper_cfg)
    for shaper_cfg in shaper_defs.INPUT_SHAPERS:
        results = helper.fit_shaper(
            shaper_cfg, cd,
            shaper_freqs=None,           # default range, 0.2 Hz step
            damping_ratio=None,
            scv=5.0,                     # Klipper default square_corner_velocity
            max_smoothing=None,
            test_damping_ratios=None,
            max_freq=None,
        )
        best = results[0]                # element 0 = library's preferred fit
        rows.append((
            best.name,
            float(best.freq),
            float(best.vibrs) * 100.0,
            float(best.smoothing),
            shaper_cfg,
        ))

    if not rows:
        print(f"PM_USB_RESULT AXIS={axis.upper()} ERROR=no_fits")
        sys.exit(0)

    # 2. Raw PSD peak for ground-truth resonance location.
    peak_f, q = measure_psd_peak(csv, axis, fs, fe)

    # 3. Filter + score.
    in_band = [r for r in rows
               if (fs + EDGE_MARGIN_HZ) <= r[1] <= (fe - EDGE_MARGIN_HZ)]
    good = [r for r in in_band if r[3] <= MAX_SMOOTHING]
    if good:
        pool, tag = good, ""
    elif in_band:
        pool, tag = in_band, " HIGH_SMOOTHING_FALLBACK=1"
    else:
        pool, tag = rows, " EDGE_FALLBACK=1"

    def score(r):
        s = r[2] + SMOOTHING_WEIGHT * r[3]
        if peak_f is not None:
            s += PEAK_DISTANCE_WEIGHT * abs(r[1] - peak_f)
        return s
    pick = min(pool, key=score)

    # 4. Re-fit ONLY the chosen shaper at fine 0.05 Hz grid for max precision.
    pick_cfg = pick[4]
    coarse_freq = pick[1]
    fine_lo = max(coarse_freq - 1.5, pick_cfg.min_freq)
    fine_hi = coarse_freq + 1.5
    fine_results = helper.fit_shaper(
        pick_cfg, cd,
        shaper_freqs=(fine_lo, fine_hi, FINE_FREQ_STEP),
        damping_ratio=None,
        scv=5.0,
        max_smoothing=None,
        test_damping_ratios=None,
        max_freq=None,
    )
    fine_best = min(fine_results, key=lambda r: r.vibrs)
    final_freq = float(fine_best.freq)
    final_vib_pct = float(fine_best.vibrs) * 100.0
    final_smooth = float(fine_best.smoothing)

    # 5. Emit single-line summary at sub-Hz precision.
    extras = ""
    if peak_f is not None:
        extras += f" peak={peak_f:.2f}Hz"
        if q is not None and q != float("inf"):
            extras += f" Q={q:.1f}"
    print(
        f"PM_USB_RESULT AXIS={axis.upper()} "
        f"shaper_type_{axis}={pick[0]} "
        f"shaper_freq_{axis}={final_freq:.2f} "
        f"vib={final_vib_pct:.2f}% "
        f"smooth={final_smooth:.3f}"
        f"{extras}"
        f"{tag}"
    )


if __name__ == "__main__":
    main()
