# Input-Shaper Measurement with PrinterMonitor Board

Use the custom ESP32-C3 board as a toolhead accelerometer to measure a 3D
printer's mechanical resonances and compute Klipper `[input_shaper]` values.

## Hardware

- ESP32-C3-MINI-1-N4 PrinterMonitor PCB
- BMI270 IMU on I²C (SDA=GPIO1, SCL=GPIO0, addr 0x68)
- WS2812B status LED (GPIO5): blue=boot, red=idle, green=capturing
- Slide switch SW3 (GPIO2): HIGH = capture, LOW = idle

Mount the board **rigidly** to the toolhead (double-sided tape or a printed
mount). Orient one of the PCB axes along the printer's X (or Y) axis.

## 1. Flash the shaper firmware

```bash
cd /Users/oleh/Desktop/PrinterMonitor
pio run -e shaper -t upload
```

The self-test is still available: `pio run -e selftest -t upload`.

## 2. Set up Python environment

```bash
conda create -n shaper python=3.11 -y
conda activate shaper
pip install pyserial numpy matplotlib
```

## 3. Capture a run

```bash
cd test/python
python3 capture.py --axis x --analyze
```

While `capture.py` is waiting:

1. On the printer, run a ringing move. Example G-code (MARLIN/Klipper):
   ```
   G28
   G1 X150 Y150 F6000
   M204 S10000         ; high accel to excite resonance
   G1 X50  F9000
   G1 X250 F9000
   G1 X50  F9000
   G1 X250 F9000
   ```
2. While the move is running, flip SW3 HIGH on the board (green LED).
3. When the move ends, flip SW3 LOW (red LED).
4. `capture.py` saves `captures/shaper_x_<timestamp>.csv` and, with
   `--analyze`, runs the FFT and shows a PSD plot.

Repeat with `--axis y` for the other axis.

## 4. Read the result

`analyze_shaper.py` prints something like:

```
X: 47.3 Hz (p=1.2e-01), 94.1 Hz (p=2.3e-02), 12.0 Hz (p=8.1e-03)
Y: 52.8 Hz (p=9.0e-02), ...

Klipper suggestion (axis=x):
  [input_shaper]
  shaper_freq_x = 47.3
  shaper_type_x = mzv
```

Put those values into `printer.cfg` under `[input_shaper]`, restart Klipper,
then re-test. Try `shaper_type_x = ei` or `2hump_ei` if you still see ringing.

## Notes / limitations

- BMI270 max accel ODR is 1600 Hz -> usable bandwidth ~700 Hz, which is far
  above the 30-80 Hz range where printer resonance lives. Comfortable headroom.
- This tool does a **resonance readout**, not the full Klipper shaper sweep
  (which compares MZV / EI / 2HUMP_EI / 3HUMP_EI residuals). For the official
  tune, Klipper's `calibrate_shaper.py` with an ADXL345 is still the gold
  standard. Use this tool for a quick diagnosis or to sanity-check changes.
- Very stiff toolheads may have resonances >100 Hz; bump `PEAK_FMAX` in
  `analyze_shaper.py` if needed.
- Keep USB cable stress-relieved to the frame so its weight doesn't pull on
  the toolhead.
