# Printer Monitor (ESPHome)

ESP32-C3 device that watches two Klipper/Moonraker printers through Home
Assistant and surfaces their state on an OLED + WS2812 LED + buttons. Also
reports printer vibration (BMI270) back to HA.

## Hardware (from `test/sketches/board_self_test.cpp`)

| Function   | GPIO |
|------------|------|
| WS2812     | 5    |
| SW1 (Mode) | 2    |
| SW2 (Act)  | 8    |
| I²C SDA    | 1    |
| I²C SCL    | 0    |

Peripherals on I²C: SSD1306 OLED `0x3C`, BMI270 IMU `0x68`.

## Install into Home Assistant

1. Copy this `esphome/` folder into your HA `config/esphome/` directory
   (or symlink it). Keep the `components/` subfolder next to the YAML.
2. `cp esphome/secrets.yaml.example esphome/secrets.yaml` and fill in Wi-Fi,
   API/OTA keys, and the entity IDs of your two Moonraker printers.
3. In HA → ESPHome Builder, open `printer-monitor.yaml` and click **Install**.
   First flash via USB, afterwards OTA.

## Expected HA entities

The YAML reads these (configurable in `secrets.yaml`) – all provided by the
Moonraker HA integration:

- `sensor.<printer>_current_print_state` (text: `printing`, `paused`, `complete`, `error`, …)
- `sensor.<printer>_progress` (0-100)
- `sensor.<printer>_extruder_temperature`
- `sensor.<printer>_bed_temperature`
- `sensor.<printer>_eta`

## UI modes (cycle with SW1)

0. Dual printer overview
1. Printer A detail
2. Printer B detail
3. System (uptime, Wi-Fi, vibration, mode)
4. Clock (from HA)

SW2 short = silence alert LED. SW2 long (≥1.5 s) = fires
`esphome.printer_monitor_long_press` event in HA – use as an automation
trigger (e.g. toggle a light, start a timelapse, etc.).

## Status LED

- Cyan slow pulse – idle
- Solid green – printing
- Amber pulse – paused
- Rainbow – print complete
- Red fast pulse – error (dims to green when silenced)

Worst state across both printers wins.

## Extending

- Add more modes: append a page + bump the modulus in `btn_mode.on_click`
  and in the `interval:` page-switch block.
- Fire more HA events from button long-presses or from vibration thresholds
  (e.g. `if vibration_rms > 0.3g then event "printer_vibrating"`).
- The vibration sensor can drive an HA automation to pause a print if
  resonance spikes – create a Numeric State automation on `sensor.vibration_rms`.

# ============================================================================
# End-to-end auto-tuning system (BMI270 + SPH0645 + GPS)
# ============================================================================

This board does more than display printer state — it also auto-tunes Klipper's
input shaper, watches print audio for anomalies, and reports its own location.

## Sensor roles

| Sensor   | Purpose                                                                            |
|----------|------------------------------------------------------------------------------------|
| BMI270   | (a) live vibration RMS dashboard sensor, (b) high-rate (1.6 kHz) capture during a Klipper chirp sweep → device-side FFT → off-device Klipper-port shaper search → applied via Moonraker. |
| SPH0645  | (a) live A-weighted dBA, (b) per-band Welford z-score; high score during a print = anomaly notification. 5-min baseline learn at print start. |
| GPS      | Reports lat/lon/altitude/HDOP/sats/fix to HA so the device "knows where it is" — useful in workshops with multiple machines or when transporting it to a friend's printer. |

## End-to-end input-shaper auto-tune

Pieces:

| Layer | File |
|-------|------|
| Klipper macros (chirp sweep, apply, persist) | [klipper/config/printer_monitor.cfg](../klipper/config/printer_monitor.cfg) |
| Klipper main config include                  | [klipper/config/printer.cfg](../klipper/config/printer.cfg) |
| Device firmware (capture + FFT + event)      | [esphome/components/bmi270/](components/bmi270/) |
| Device YAML wiring (api service, script)     | [esphome/printer-monitor.yaml](printer-monitor.yaml) |
| Off-device Klipper shaper-search port        | [test/python/shaper_calibrate.py](../test/python/shaper_calibrate.py) |
| HA-side wrapper (call Moonraker)             | [test/python/pm_apply_shaper.py](../test/python/pm_apply_shaper.py) |
| HA glue (automations, helpers, shell_command, rest_command) | [homeassistant/](../homeassistant/) |

### Workflow per axis

1. **Mount.** Cartesian convention:
   - X test: PCB on toolhead.
   - Y test: PCB on bed.
2. **Trigger.** From Home Assistant call service `esphome.printer_monitor_start_shaper_capture` with `axis: "X"`. The device:
   - allocates 24 KB capture buffers,
   - reconfigures BMI270 to 1600 Hz, no averaging filter,
   - enters `capturing` state,
   - fires `esphome.printer_monitor_request_sweep` event back to HA.
3. **Sweep.** HA automation `printer_monitor_forward_sweep_request` POSTs `PM_SHAPER_SWEEP_X` to Moonraker. Klipper chirps the toolhead from 5 → 130 Hz at the bed centre while the BMI270 records.
4. **Analyse on-device.** When the buffer fills (or 8 s elapse) the device computes a 4096-point FFT per axis, picks the dominant raw axis, and downsamples to 128 PSD bins.
5. **Publish.** Capture-state text-sensor transitions to `done`, ESPHome script `send_psd_event` packs `{axis, fs, peak_hz, psd}` into a `printer_monitor_psd_ready` event.
6. **Search off-device.** HA automation writes the event to `/tmp/pm_psd.json` and runs `pm_apply_shaper.py`, which evaluates all six Klipper shaper types (`zv`, `mzv`, `zvd`, `ei`, `2hump_ei`, `3hump_ei`) at 1 Hz steps using the Klipper formula `score = vibrations * (smoothing + 0.5)`.
7. **Apply.** The script POSTs `PM_SHAPER_APPLY TYPE_X=mzv FREQ_X=43.2` to Moonraker. The Klipper macro:
   - sets `[input_shaper]` live with `SET_INPUT_SHAPER`,
   - persists via `[save_variables]` so it survives restart,
   - logs the new values to the console.
8. **Reboot-safe.** `[delayed_gcode pm_restore_at_boot]` re-applies saved values on every Klipper restart.

## End-to-end acoustic anomaly

1. SPH0645 streams 16 kHz mono I²S samples.
2. Per second the device computes a 512-point FFT with A-weighting and
   bins energy into 16 log-spaced bands.
3. For the first 5 minutes after enabling (or whenever you call
   `start_baseline_learn` from HA), it builds Welford running mean &
   variance per band.
4. After that, every band's z-score is computed; the **maximum** band
   z-score is published as `sensor.printer_monitor_acoustic_anomaly_score`,
   with `anomaly_band` indicating which band fired.
5. HA automation `printer_monitor_acoustic_anomaly` notifies you (and
   optionally pauses) when the score exceeds 4.0 σ for 10 s while a
   printer is actively printing.

Tip: trigger `start_baseline_learn` whenever a print starts so the
baseline reflects the actual machine noise.

## GPS

Plug-and-play. The first NMEA sentences appear within ~30 s of cold start
(longer indoors). HA receives:

- `sensor.printer_monitor_gps_latitude` / `_longitude` (decimal degrees)
- `sensor.printer_monitor_gps_altitude` (m)
- `sensor.printer_monitor_gps_satellites` / `_hdop`
- `text_sensor.printer_monitor_gps_fix_status`

## OLED page map (updated)

| Page | What it shows |
|------|---------------|
| 0    | Dual printer overview |
| 1    | Printer A detail |
| 2    | Printer B detail |
| 3    | System (vibration RMS, RSSI, uptime, switches) |
| 4    | Clock |
| 5    | Shaper capture state, axis, progress, last peak Hz |
| 6    | Sound level (dBA) and acoustic anomaly bar |
| 7    | GPS status (fix, sats, lat, lon, HDOP) |

The slide-switch SW3 still selects between Printer A and Printer B detail
views; long-press your designated trigger (or call `script.turn_on
script.printer_monitor_shaper_x` from HA) to start a sweep.

## Troubleshooting

- **BMI270 capture never fills**: confirm 1.6 kHz ODR is reaching the
  driver — log `bmi270` at DEBUG. The de-dup loop in `capture_step_()`
  drops repeated samples; if your I²C bus is slow you'll fill in ≈3 s
  instead of 2.5 s but the FFT will still resolve.
- **Anomaly score always 0**: baseline is still learning; wait 5 min or
  manually call `freeze_baseline()` after enough quiet samples.
- **GPS shows `no_fix`**: ensure antenna sees the sky; the ATGM332D needs
  ≥4 satellites to commit a fix. HDOP < 5 is good.
- **Shaper sweep clips**: the macro auto-clamps displacement to 0.20 –
  25 mm using your max_velocity / max_accel; if it still moves outside the
  bed, lower `END_FREQ` in `PM_SHAPER_SWEEP_X` to e.g. 90.

