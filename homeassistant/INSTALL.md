# Install — PrinterMonitor in Home Assistant

## 1. Copy Python scripts into HA

```
/config/python/pm_apply_shaper.py
/config/python/pm_shaper_calibrate.py
```

(Source: `test/python/pm_apply_shaper.py` and `test/python/shaper_calibrate.py`)

## 2. Edit `/config/configuration.yaml`

Append the following:

```yaml
shell_command: !include shell_commands.printer_monitor.yaml
input_boolean: !include_dir_merge_named input_booleans/
input_select:  !include_dir_merge_named input_selects/
automation pm: !include automations.printer_monitor.yaml
```

Or, if you already use `automation: !include automations.yaml`, just append the
contents of `automations.printer_monitor.yaml` to that file.

## 3. Configure Moonraker URL

In `automations.printer_monitor.yaml`, edit `moonraker_url:` (default
`http://klipperpi:7125`) to match your Klipper host.

## 4. Reload HA

Settings → System → Restart, or:
```
ha core restart
```

## 5. Use it

- **Acoustic anomaly**: as soon as `binary_sensor.<klipper>_printing` turns on,
  the device starts a 5-min baseline learning window. After that it scores
  every 1 s. If `sensor.printer_monitor_acoustic_anomaly_score > 4` for 10 s,
  HA notifies you.
- **Shaper auto-tune**:
    1. Mount printer monitor on the toolhead (X test) or on the bed (Y test).
    2. From HA, run script `script.printer_monitor_shaper_x` (or `_y`).
       That calls the device's `start_shaper_capture` API service, which
       starts BMI270 capture AND fires an event back to HA, which calls
       Moonraker to run `PM_SHAPER_SWEEP_X` (or `_Y`) on the printer.
    3. Device publishes `printer_monitor_psd_ready` ~8 s later.
    4. Automation pipes PSD JSON to `pm_apply_shaper.py` which calls
       Moonraker `PM_SHAPER_APPLY` to save+activate the shaper.

