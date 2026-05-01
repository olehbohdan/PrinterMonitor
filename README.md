# PrinterMonitor

> A multi-sensor edge-AI companion for 3D-printer farms — built on a custom ESP32-C3 PCB, ESPHome, Home Assistant, and Klipper/Moonraker.

PrinterMonitor is a small, always-on hardware companion that lives on a 3D-printer bench and watches every printer in the room. It surfaces dual-printer state on a 0.96″ OLED + RGB LED, runs a continuous **acoustic anomaly detector** to catch print failures, and acts as a **hands-off input-shaper auto-tuner** for Klipper — all on the local network, no cloud dependencies, no subscriptions.

| | |
|---|---|
| **Hardware** | Custom 50×40 mm 2-layer PCB · ESP32-C3 · BMI270 IMU · SPH0645 I²S mic · ATGM332D GPS · SSD1306 OLED · WS2812 RGB · 2× SPDT slide |
| **Edge SW** | ESPHome (Arduino framework) + 3 custom external components (`bmi270`, `sph0645`, `gps_uart`) doing on-device FFT, Welford z-score, NMEA parsing |
| **Off-device** | Home Assistant 2025.4 (native API), Moonraker REST, Klipper macros (`PM_USB_TUNE_X/Y`), Python helper scripts |
| **BoM cost** | ≈ USD 26 in qty 1, ≈ USD 18 at 100-unit scale |

---

## Why this exists

Existing per-printer monitoring solutions each solve at most one of these problems, and almost always require an internet round-trip:

| Solution | Local-only? | Multi-printer? | Auto input-shaper? | Failure detection |
|---|:---:|:---:|:---:|:---:|
| OctoPrint + plug-ins | ✅ | ❌ (one Pi each) | ❌ | None built-in |
| Bambu Handy app | ❌ cloud | ❌ | n/a (factory tuned) | Vendor camera only |
| Obico / Spaghetti Detective | ❌ cloud | ✅ | ❌ | $4–8/mo cloud ML |
| Klipper ADXL345 macros | ✅ | ❌ | Manual SSH dance | None |
| **PrinterMonitor (this repo)** | ✅ | ✅ | **Hands-off** | **Edge-AI acoustic** |

It is the first single, sub-\$30 device that does dashboarding **+** calibration **+** anomaly detection together, fully on-LAN, with three sensors fused on-device before being handed off to Home Assistant.

---

## Repository layout

```
PrinterMonitor/
├── docs/
│   ├── overleaf/main.tex          ← LaTeX final report (paste into Overleaf)
│   └── USB_SHAPER_TUNING.md       ← USB-tether tuning protocol notes
├── esphome/
│   ├── printer-monitor.yaml       ← Main ESPHome config
│   ├── secrets.yaml.example       ← Template (fill in & rename to secrets.yaml)
│   └── components/
│       ├── bmi270/                ← IMU + 4096-pt FFT + per-axis PSD events
│       ├── sph0645/               ← I²S mic, dBA + per-band Welford z-score
│       └── gps_uart/              ← NMEA-0183 parser
├── homeassistant/
│   ├── automations.printer_monitor.yaml   ← 3 automations (sweep / PSD / anomaly)
│   ├── helpers.printer_monitor.yaml       ← input_boolean, input_select
│   ├── shell_commands.printer_monitor.yaml
│   ├── rest_commands.printer_monitor.yaml
│   └── INSTALL.md                         ← Drop-in HA install steps
├── klipper/
│   ├── config/                    ← printer.cfg, printer_monitor.cfg, moonraker.conf
│   └── scripts/                   ← pm_usb_capture.py, pm_usb_recommend.py
├── test/
│   ├── sketches/                  ← board_self_test.cpp, rainbow.cpp, input_shaper_capture.cpp
│   └── python/                    ← shaper_calibrate.py port + analysis tools
├── platformio.ini                 ← 3 envs: rainbow, selftest, shaper
└── Netlist_INClassFirstPCB_2026-04-18.enet  ← EasyEDA Pro netlist
```

---

## System architecture

```
+---------------------------------+
| Layer 4 — User                  |   OLED · WS2812 LED · slide switches
+---------------------------------+
| Layer 3 — Off-device            |   Home Assistant automations
|                                 |   Moonraker REST API
|                                 |   Bambu cloud → HA Bambu integration
+---------------------------------+
| Layer 2 — Edge MCU              |   ESP32-C3 (custom PCB)
|   ESPHome native API ── Wi-Fi → HA
|   USB-Serial-JTAG ──── USB ──→ Klipper Pi
|   FFT / PSD / RMS / dBA / z-score on-device
+---------------------------------+
| Layer 1 — Sensors               |   BMI270 · SPH0645 · ATGM332D GPS
+---------------------------------+
```

Three independent data flows coexist on the same MCU:

1. **Display flow** *(Wi-Fi, read-only)* — HA pushes printer-state sensors over the ESPHome native API; the device renders them on the OLED at 500 ms refresh.
2. **Calibration flow** *(USB, bidirectional)* — `PM_USB_TUNE_X` Klipper macro → `pm_usb_capture.py` → device streams 1.6 kHz BMI270 samples → `calibrate_shaper.py` picks shaper → applied via Moonraker.
3. **Anomaly flow** *(Wi-Fi, push-only)* — SPH0645 → 1024-pt FFT → 8 log-mel bands → Welford running-stat → if any band > 4 σ for ≥3 s during an active print → `esphome.printer_monitor_acoustic_anomaly` event → mobile-app notification.

---

## Hardware

### Pinout

| Net / signal | ESP32-C3 GPIO | Peripheral | Notes |
|---|---|---|---|
| I²C SDA | GPIO1 | OLED + BMI270 | 400 kHz |
| I²C SCL | GPIO0 | OLED + BMI270 | |
| UART0 RX | GPIO20 | GPS TX | 9600 baud, NMEA-0183 |
| UART0 TX | GPIO21 | GPS RX | |
| I²S BCLK | GPIO10 | SPH0645 BCLK | 16 kHz / 24-bit, L-channel |
| I²S WS / LRCL | GPIO3 | SPH0645 WS | |
| I²S DIN | GPIO7 | SPH0645 DOUT | |
| WS2812 DIN | GPIO5 | WS2812B | RMT-driven |
| SW1 (printer select) | GPIO2 | SPDT slide | Pull-up + 30 ms debounce |
| SW2 (silence) | GPIO8 | SPDT slide | Boot-strap-safe (HIGH at boot) |
| USB D± | D+/D− | USB-Serial-JTAG | Power + capture stream |

### Bill of materials

| Ref | Part | Function | Qty | USD |
|---|---|---|---:|---:|
| U1 | ESP32-C3-MINI-1U | MCU + Wi-Fi/BLE | 1 | 2.50 |
| U2 | ATGM332D-5N31 | GNSS UART receiver | 1 | 3.80 |
| U3 | BMI270 (SparkFun) | 6-axis IMU @ 1.6 kHz | 1 | 8.95 |
| U4 | SSD1306 0.96″ OLED | 128×64 dashboard | 1 | 3.20 |
| U5 | SPH0645LM4H | I²S MEMS mic | 1 | 2.80 |
| D1 | WS2812B | Status LED | 1 | 0.30 |
| SW1/2 | C&K JS-series SPDT | Mode / silence | 2 | 0.45 |
| | 0.1/10 µF MLCC, 10 kΩ | Decoupling + pull-ups | ~10 | 0.40 |
| | Custom 2-layer PCB | 50×40 mm | 1 | 2.00 |
| | USB-C plug + cable | Power + data | 1 | 1.50 |
| | | **Total** | | **≈ 26.35** |

The original EasyEDA Pro netlist is preserved at [`Netlist_INClassFirstPCB_2026-04-18.enet`](Netlist_INClassFirstPCB_2026-04-18.enet).

---

## Quick start

### 1 · Build & flash the firmware (ESPHome path — recommended)

```bash
# clone and prepare secrets
git clone https://github.com/<you>/PrinterMonitor.git
cd PrinterMonitor/esphome
cp secrets.yaml.example secrets.yaml

# generate API/OTA keys
python3 -c "import secrets,base64;print(base64.b64encode(secrets.token_bytes(32)).decode())"
# paste into api_key + ota_password fields, then fill in:
#   wifi_ssid / wifi_password
#   printer_a_*  → your Klipper sensor entity_ids
#   printer_b_*  → your Bambu sensor entity_ids
```

First flash over USB:

```bash
pip install esphome
esphome run printer-monitor.yaml --device /dev/cu.usbmodem1101
```

Subsequent flashes go OTA over Wi-Fi automatically.

### 2 · Wire up Home Assistant

Drop the four files in `homeassistant/` into your HA `/config/` directory and follow [homeassistant/INSTALL.md](homeassistant/INSTALL.md). It adds:

- a `shell_command` that calls `pm_apply_shaper.py`
- three automations: `forward_sweep_request`, `process_psd_when_capture_completes`, `notify_on_acoustic_anomaly`
- helpers (`input_boolean.printer_monitor_silent`, `input_select.printer_monitor_axis`)

### 3 · Wire up Klipper (only if you want USB shaper tuning)

```bash
scp klipper/scripts/pm_usb_capture.py pi@klipperpi:~/printer_data/config/printer_monitor/
scp klipper/config/printer_monitor.cfg pi@klipperpi:~/printer_data/config/
ssh pi@klipperpi '~/klippy-env/bin/pip install pyserial && systemctl restart klipper'
```

Then plug the PrinterMonitor PCB directly into the Klipper Pi via USB and run from the Klipper console:

```
PM_USB_TUNE_X CALIBRATE=1
PM_USB_TUNE_Y CALIBRATE=1
```

Full protocol details: [docs/USB_SHAPER_TUNING.md](docs/USB_SHAPER_TUNING.md).

### 4 · Alternative: PlatformIO sanity-test sketches

Three Arduino-framework sketches live under [test/sketches/](test/sketches/) for hardware bring-up:

```bash
pio run -e selftest -t upload    # tests LED, switches, IMU, OLED, GPS, mic
pio run -e rainbow  -t upload    # WS2812 demo
pio run -e shaper   -t upload    # input-shaper USB capture firmware (no ESPHome)
```

---

## Software components

### On-device (edge)

The ESPHome YAML wires three custom external components together. Each one is documented in its own header:

- **[`bmi270/`](esphome/components/bmi270/)** — Polling IMU @ 100 Hz for live RMS dashboard. On `start_capture(axis)` reconfigures to 1.6 kHz, captures 4096 samples, runs a 4096-pt real FFT, computes per-axis PSD, and exposes the result via three `homeassistant.event` calls (`printer_monitor_psd_ready`, one per chip axis).
- **[`sph0645/`](esphome/components/sph0645/)** — I²S DMA → 1024-pt FFT → 8 log-mel bands → Welford running mean/variance with 5-min baseline learn at print start. Publishes `Sound Level (dBA)`, `Anomaly Score`, and `Anomaly Band` sensors.
- **[`gps_uart/`](esphome/components/gps_uart/)** — Pure-Arduino NMEA-0183 parser. Tolerates GP/GN/GL/BD prefixes; only publishes when fields are valid (so `lat/lon` stay `unknown` until a fix is acquired).

### Off-device

- **`homeassistant/automations.printer_monitor.yaml`** — Three automations stitching the device into HA workflows.
- **`test/python/shaper_calibrate.py`** — Vendored from upstream Klipper. Used by HA's `pm_apply_shaper.py` to pick the optimal shaper (mzv / zv / zvd / ei / 2hump_ei / 3hump_ei) given a PSD.
- **`klipper/scripts/pm_usb_capture.py`** — Host-side USB serial reader. Speaks `PING`/`PONG` and `START`/`STOP` to the device, decodes 8-byte frames at 12.8 kB/s, writes Klipper-compatible `/tmp/pm_resonances_<axis>.csv`.

---

## Experimental results

| Metric | Result |
|---|---|
| **HA → OLED latency** | 451 ± 53 ms (n = 5; bound by 500 ms display refresh) |
| **Input-shaper peak vs. ADXL345 reference** | < 1 Hz error, both axes; same shaper recommended |
| **Acoustic anomaly detection latency** | 3 s after fault injection (X-belt loosened) |
| **False-positive rate** | 0 across 7-day continuous run |
| **Flash used** | 1.16 MB / 4 MB |
| **Heap free** | ~118 KB steady-state |
| **CPU load** | ~28 % avg, ~85 % during FFT |
| **Power** | 74 mA @ 5 V (0.37 W) |
| **Boot time** | 3.1 s incl. Wi-Fi + HA handshake |

Full methodology, tables, and discussion of failure modes are in the LaTeX report at [docs/overleaf/main.tex](docs/overleaf/main.tex).

---

## OLED page guide

| Page | SW1 select | Contents |
|---|---|---|
| 0 | Ender (left) | Dual overview — both printers, progress bars, temps |
| 1 | Ender (left) | Printer A full — large %, ETA, E/B temps |
| 2 | A1 (right) | Printer B full — same layout |
| 3 | (auto) | System — uptime, RSSI, vibration RMS, mode |
| 4 | (auto) | Clock from HA |
| 5 | (auto) | Shaper capture progress |
| 6 | (auto) | Acoustic anomaly score + band |

LED meaning: **cyan slow pulse** = idle · **solid green** = printing · **amber pulse** = paused · **rainbow** = complete · **red fast pulse** = error (dims to green when SW2 silenced).

---

## Limitations & roadmap

- Acoustic anomaly is *presence/absence only* — a small TFLite-Micro classifier (<200 KB) on the C3 is the natural next iteration to label the fault type.
- Input-shaper tuning still requires USB tether; loop()-driven sampling on the C3 cannot hold deterministic 1.6 kHz over Wi-Fi.
- Bambu A1 progress depends on the Bambu cloud bridge; LAN-only MQTT firmware support is a future-work item.

---

## License

MIT for the code and ESPHome configs. The `test/python/shaper_calibrate.py` file is vendored from the [Klipper project](https://github.com/Klipper3d/klipper) under GPLv3.

---

## Acknowledgements

- ESPHome and Home Assistant communities
- Klipper3d for `calibrate_shaper.py`
- SparkFun for the BMI270 Arduino library
- Course staff for the in-class PCB exercise this device evolved from
