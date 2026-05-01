#!/usr/bin/env python3
"""Query Spoolman for active spool material and update Klipper FILAMENT_VARS."""
import json
import sys
import urllib.request

MOONRAKER = "http://localhost:7125"
SPOOLMAN = "http://localhost:7912"


def get_json(url):
    try:
        with urllib.request.urlopen(url, timeout=5) as r:
            return json.loads(r.read())
    except Exception as e:
        print(f"Error fetching {url}: {e}")
        return None


def send_gcode(script):
    data = json.dumps({"script": script}).encode()
    req = urllib.request.Request(
        f"{MOONRAKER}/printer/gcode/script",
        data=data,
        headers={"Content-Type": "application/json"},
    )
    try:
        urllib.request.urlopen(req, timeout=5)
    except Exception:
        pass


# Get active spool ID from Moonraker
resp = get_json(f"{MOONRAKER}/server/spoolman/spool_id")
spool_id = (resp or {}).get("result", {}).get("spool_id")
if not spool_id:
    print("No active spool selected")
    sys.exit(0)

# Get spool details directly from Spoolman
spool = get_json(f"{SPOOLMAN}/api/v1/spool/{spool_id}")
if not spool:
    print(f"Could not fetch spool #{spool_id} from Spoolman")
    sys.exit(0)

filament = spool.get("filament", {})
material = (filament.get("material") or "").upper()

if not material:
    print(f"No material set for spool #{spool_id}")
    sys.exit(0)

vendor = (filament.get("vendor") or {}).get("name", "Unknown")
name = filament.get("name", "")
print(f"Spool #{spool_id}: {vendor} {name} ({material})")

gcode = f"SET_FILAMENT TYPE={material}"
if "--unload" in sys.argv:
    gcode += "\n_DO_FILAMENT_UNLOAD"
elif "--load" in sys.argv:
    gcode += "\n_DO_FILAMENT_LOAD"
send_gcode(gcode)
