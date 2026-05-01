---
applyTo: "**"
---

# PrinterMonitor — Continuation Instructions

You are continuing an in-progress project. Read this file first, then proceed.

## Project goal

An ESP32-C3 IoT device that monitors **two Klipper/Moonraker 3D printers**
through Home Assistant. Features:

- OLED dashboard (5 pages: dual overview, printer A, printer B, system, clock)
- WS2812 LED status indicator (idle / printing / paused / complete / error)
- 2 buttons (SW1 cycles modes, SW2 silences alerts / fires HA events on long-press)
- BMI270 accelerometer → vibration RMS reported to HA
- All integration via **ESPHome native API** (not MQTT)

## Current state

Already built under `esphome/`:

- [esphome/printer-monitor.yaml](esphome/printer-monitor.yaml) — main config
- [esphome/secrets.yaml.example](esphome/secrets.yaml.example) — template
- [esphome/components/bmi270/](esphome/components/bmi270/) — custom external component
  using SparkFun BMI270 Arduino lib (C++ + Python codegen)
- [esphome/README.md](esphome/README.md) — setup notes

Hardware pinout (from [test/sketches/board_self_test.cpp](test/sketches/board_self_test.cpp)):
WS2812=GPIO5, SW1=GPIO2, SW2=GPIO8, I²C SDA=GPIO1 / SCL=GPIO0,
OLED SSD1306 @ 0x3C, BMI270 @ 0x68.

## Home Assistant MCP

Home Assistant is at `http://192.168.1.96:8123`. The user has installed the
**jarahkon.hass-mcp-server** VS Code extension which exposes 66 `ha_*` tools
(ha_get_states, ha_call_service, ha_list_entity_registry, etc.).

Verify the tools are available in your tool list before proceeding. If they
are, continue with the tasks below. If not, tell the user to enable them in
the Copilot Chat tools picker.

## What to do in this new chat

1. **Find the two printers.** Call `ha_list_entity_registry` or
   `ha_get_states` filtered to `sensor` domain. Look for Moonraker-style
   entity IDs — likely patterns:
   - `sensor.<printer>_current_print_state`
   - `sensor.<printer>_progress`
   - `sensor.<printer>_extruder_temperature`
   - `sensor.<printer>_bed_temperature`
   - `sensor.<printer>_eta`

   If the printers use different integrations (OctoPrint, Bambu, etc.) map
   to equivalent entities and update the YAML's `homeassistant` sensor
   platform entries accordingly.

2. **Write `esphome/secrets.yaml`** (do NOT commit — it's gitignored).
   Fill in the discovered entity IDs for both printers. Wi-Fi SSID/password
   and API/OTA keys should be placeholders the user fills in:
   - Generate API key: `python3 -c "import secrets,base64;print(base64.b64encode(secrets.token_bytes(32)).decode())"`

3. **Validate the YAML.** If ESPHome CLI is available, run
   `esphome config esphome/printer-monitor.yaml`. Otherwise do a static
   review: check entity IDs exist, unit_of_measurement matches, text_sensor
   vs sensor types are correct (state is text, progress/temps are numeric).

4. **Add HA-side automation scaffolding** using MCP tools:
   - Create an `input_boolean.printer_monitor_silent` helper so the device
     can mirror its silence state to HA.
   - Create a sample automation that reacts to
     `esphome.printer_monitor_long_press` event (the device fires this on
     SW2 long-press). Suggest something useful like toggling exhaust fan
     or sending a mobile notification with printer status.

5. **Verify & report back** to the user with:
   - Entity IDs found (both printers)
   - Path to the written secrets.yaml
   - Any HA helpers/automations created
   - Flashing instructions reminder (first flash USB at `/dev/cu.usbmodem1101`,
     then OTA afterwards)

## Conventions & gotchas

- Do NOT add markdown docs unless asked.
- Keep the BMI270 external component as-is; it already works with
  Arduino framework.
- If an entity isn't found, still write secrets.yaml but leave that field
  with a clear `TODO:` comment and flag it to the user.
- ESPHome `homeassistant` platform requires entities be **exposed** to
  the Assist API in HA (Settings → Voice assistants → Expose), OR just
  let ESPHome pull by entity_id (that does not require exposure — confirm
  this with docs if unsure).
- The printer state classifier in the YAML's interval lambda uses
  substring matching on the state text — if Moonraker uses unusual state
  strings (`standby`, `ready`, `cancelled`, etc.) extend the classifier.

## Files to touch

- Create: `esphome/secrets.yaml`
- Maybe edit: `esphome/printer-monitor.yaml` (sensor platforms if entity
  naming differs from the template's default assumptions)
- Nothing else should change unless the user requests new features.
