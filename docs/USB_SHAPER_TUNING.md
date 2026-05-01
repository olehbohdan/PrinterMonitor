# PrinterMonitor — USB Shaper Tuning

The wifi/HA capture path is fine for casual vibration monitoring but
loop()-driven sampling on the C3 can't hit deterministic 1.6 kHz. For
real input-shaper tuning we stream raw BMI270 samples over USB to the
Klipper Pi where Klipper's stock `calibrate_shaper.py` does the math.

## Wiring

Plug the PrinterMonitor PCB **directly into the Klipper Pi via USB**.
It enumerates as `/dev/ttyACM*`.  No reset needed — the firmware always
listens for the `START` command on USB-Serial-JTAG.

When unplugged from the Pi the board falls back to its normal display
duties over wifi; the USB stack is idle.

## One-time host setup (Klipper Pi)

```bash
# 1. Place capture script
mkdir -p ~/printer_data/config/printer_monitor
scp pi@klipperpi:- < klipper/scripts/pm_usb_capture.py \
    ~/printer_data/config/printer_monitor/pm_usb_capture.py
chmod +x ~/printer_data/config/printer_monitor/pm_usb_capture.py

# 2. Install pyserial inside the Klippy venv (so RUN_SHELL_COMMAND finds it)
~/klippy-env/bin/pip install pyserial

# 3. Drop the cfg in (or merge into your existing printer_monitor.cfg)
scp klipper/config/printer_monitor.cfg \
    pi@klipperpi:~/printer_data/config/printer_monitor.cfg

# 4. Restart Klipper
```

The cfg file is a superset of the wifi-path macros — it adds:

| Macro                | Purpose                                       |
| -------------------- | --------------------------------------------- |
| `PM_USB_TUNE_X`      | Capture-while-chirp on X, write CSV           |
| `PM_USB_TUNE_X CALIBRATE=1` | Same + run `calibrate_shaper.py`       |
| `PM_USB_TUNE_Y` / `..._Y CALIBRATE=1` | same for Y                  |
| `PM_USB_TUNE_BOTH`   | X then Y back-to-back                         |

Output ends up at `/tmp/pm_resonances_<axis>.csv`.  The Klipper console
echoes the python script's stdout including a line like:

```
PM_USB_CAPTURE AXIS=X SAMPLES=4000 FS=1600 CSV=/tmp/pm_resonances_x.csv
```

If `CALIBRATE=1`, the recommended shaper / freq is printed in the
familiar Klipper format.

## Wire protocol (firmware side)

Text in (newline-terminated):

| Command  | Effect                                            |
| -------- | ------------------------------------------------- |
| `PING`   | Replies `PONG\n`                                  |
| `START`  | Switches BMI270 to 1600 Hz, replies `OK\n`, then continuously emits binary frames |
| `STOP`   | Stops streaming, returns to 100 Hz, replies `STOPPED\n` |

Binary frames out, 8 bytes each:

```
0xAA 0x55  ax_lo ax_hi  ay_lo ay_hi  az_lo az_hi
```

`int16` little-endian raw LSBs at ±8 g full scale (`32768 LSB / 8 g`).

12.8 kB/s sustained — well within USB-Serial-JTAG's 4 KB TX FIFO and
the host's tolerance for `pyserial`-paced reads.
