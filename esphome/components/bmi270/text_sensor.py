"""Text-sensor platform for BMI270 capture state.

Usage:
    text_sensor:
      - platform: bmi270
        bmi270_id: bmi
        capture_state: {name: "Shaper Capture State"}
        capture_axis:  {name: "Shaper Capture Axis"}
"""
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import text_sensor
from esphome.const import CONF_ID

from . import sensor as bmi_sensor  # reuse class registration

DEPENDENCIES = []
AUTO_LOAD = ["text_sensor"]

CONF_BMI270_ID = "bmi270_id"
CONF_CAPTURE_STATE = "capture_state"
CONF_CAPTURE_AXIS  = "capture_axis"

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_BMI270_ID): cv.use_id(bmi_sensor.BMI270Component),
        cv.Optional(CONF_CAPTURE_STATE): text_sensor.text_sensor_schema(),
        cv.Optional(CONF_CAPTURE_AXIS):  text_sensor.text_sensor_schema(),
    }
)


async def to_code(config):
    parent = await cg.get_variable(config[CONF_BMI270_ID])
    if CONF_CAPTURE_STATE in config:
        s = await text_sensor.new_text_sensor(config[CONF_CAPTURE_STATE])
        cg.add(parent.set_capture_state(s))
    if CONF_CAPTURE_AXIS in config:
        s = await text_sensor.new_text_sensor(config[CONF_CAPTURE_AXIS])
        cg.add(parent.set_capture_axis_sensor(s))
