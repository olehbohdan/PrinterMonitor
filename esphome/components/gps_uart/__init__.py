"""ATGM332D GPS NMEA parser - top-level component.

Usage in YAML:
    gps_uart:
      uart_id: uart_bus
      latitude:    {name: "GPS Latitude"}
      longitude:   {name: "GPS Longitude"}
      altitude:    {name: "GPS Altitude"}
      hdop:        {name: "GPS HDOP"}
      satellites:  {name: "GPS Satellites"}
      speed:       {name: "GPS Speed"}
      fix_status:  {name: "GPS Fix Status"}
      utc_time:    {name: "GPS UTC Time"}
"""
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import sensor, text_sensor, uart
from esphome.const import (
    CONF_ID,
    CONF_LATITUDE,
    CONF_LONGITUDE,
    CONF_ALTITUDE,
    STATE_CLASS_MEASUREMENT,
    UNIT_DEGREES,
    UNIT_METER,
)

CODEOWNERS = ["@local"]
DEPENDENCIES = ["uart"]
AUTO_LOAD = ["sensor", "text_sensor"]
MULTI_CONF = False

ns = cg.esphome_ns.namespace("gps_uart")
GPSComponent = ns.class_("GPSComponent", cg.Component, uart.UARTDevice)

CONF_HDOP = "hdop"
CONF_SATELLITES = "satellites"
CONF_SPEED = "speed"
CONF_FIX_STATUS = "fix_status"
CONF_UTC_TIME = "utc_time"


def _num(unit, dec):
    return sensor.sensor_schema(
        unit_of_measurement=unit,
        accuracy_decimals=dec,
        state_class=STATE_CLASS_MEASUREMENT,
    )


CONFIG_SCHEMA = (
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(GPSComponent),
            cv.Optional(CONF_LATITUDE):   _num(UNIT_DEGREES, 6),
            cv.Optional(CONF_LONGITUDE):  _num(UNIT_DEGREES, 6),
            cv.Optional(CONF_ALTITUDE):   _num(UNIT_METER, 1),
            cv.Optional(CONF_HDOP):       _num("", 2),
            cv.Optional(CONF_SATELLITES): _num("", 0),
            cv.Optional(CONF_SPEED):      _num("km/h", 2),
            cv.Optional(CONF_FIX_STATUS): text_sensor.text_sensor_schema(),
            cv.Optional(CONF_UTC_TIME):   text_sensor.text_sensor_schema(),
        }
    )
    .extend(cv.COMPONENT_SCHEMA)
    .extend(uart.UART_DEVICE_SCHEMA)
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await uart.register_uart_device(var, config)

    for k, setter in [
        (CONF_LATITUDE,   "set_lat"),
        (CONF_LONGITUDE,  "set_lon"),
        (CONF_ALTITUDE,   "set_alt"),
        (CONF_HDOP,       "set_hdop"),
        (CONF_SATELLITES, "set_sats"),
        (CONF_SPEED,      "set_speed"),
    ]:
        if k in config:
            s = await sensor.new_sensor(config[k])
            cg.add(getattr(var, setter)(s))

    if CONF_FIX_STATUS in config:
        s = await text_sensor.new_text_sensor(config[CONF_FIX_STATUS])
        cg.add(var.set_fix_status(s))
    if CONF_UTC_TIME in config:
        s = await text_sensor.new_text_sensor(config[CONF_UTC_TIME])
        cg.add(var.set_utc_time(s))
