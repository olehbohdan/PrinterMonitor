# PrinterMonitor — Home Assistant integration

Drop these files into your Home Assistant `/config/` directory (Samba/SSH/File-Editor add-on),
then add the include lines shown in `INSTALL.md` to your `configuration.yaml`.

## Files

- `pm_apply_shaper.py`   — copy of `test/python/pm_apply_shaper.py`. Runs after
  the device sends a `printer_monitor_psd_ready` event. Picks the best Klipper
  shaper and pushes it to Moonraker.
- `pm_shaper_calibrate.py` — copy of `test/python/shaper_calibrate.py`
  (the Klipper-port that `pm_apply_shaper.py` imports).
- `automations.printer_monitor.yaml` — three automations:
    1. `Printer Monitor: forward sweep request to Moonraker`
    2. `Printer Monitor: process PSD when capture completes`
    3. `Printer Monitor: notify on acoustic anomaly during print`
- `helpers.printer_monitor.yaml` — input_boolean / input_select helpers.
- `shell_commands.printer_monitor.yaml` — single shell_command line.

See `INSTALL.md` for the exact configuration.yaml include block.
