"""SPH0645 I2S MEMS microphone (Adafruit, mono, 24-bit) for ESPHome.

Top-level component (NOT a sensor: platform). Usage:
    sph0645:
      id: mic
      bclk_pin: GPIO10
      ws_pin:   GPIO3
      din_pin:  GPIO7
      sample_rate: 16000
      dba:           {name: "Sound Level"}
      rms:           {name: "Sound RMS"}
      anomaly_score: {name: "Acoustic Anomaly Score"}
      anomaly_band:  {name: "Anomaly Band"}
"""
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import sensor
from esphome.const import (
    CONF_ID,
    STATE_CLASS_MEASUREMENT,
)

CODEOWNERS = ["@local"]
AUTO_LOAD = ["sensor"]
MULTI_CONF = False

sph0645_ns = cg.esphome_ns.namespace("sph0645")
SPH0645Component = sph0645_ns.class_("SPH0645Component", cg.PollingComponent)

CONF_BCLK_PIN = "bclk_pin"
CONF_WS_PIN = "ws_pin"
CONF_DIN_PIN = "din_pin"
CONF_SAMPLE_RATE = "sample_rate"
CONF_DBA = "dba"
CONF_RMS = "rms"
CONF_ANOMALY_SCORE = "anomaly_score"
CONF_ANOMALY_BAND = "anomaly_band"

DBA_SCHEMA = sensor.sensor_schema(
    unit_of_measurement="dB",
    accuracy_decimals=1,
    state_class=STATE_CLASS_MEASUREMENT,
)
NUM_SCHEMA = sensor.sensor_schema(
    accuracy_decimals=2,
    state_class=STATE_CLASS_MEASUREMENT,
)

CONFIG_SCHEMA = (
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(SPH0645Component),
            cv.Optional(CONF_BCLK_PIN, default=10): cv.int_range(0, 21),
            cv.Optional(CONF_WS_PIN, default=3):    cv.int_range(0, 21),
            cv.Optional(CONF_DIN_PIN, default=7):   cv.int_range(0, 21),
            cv.Optional(CONF_SAMPLE_RATE, default=16000): cv.int_range(8000, 48000),
            cv.Optional(CONF_DBA): DBA_SCHEMA,
            cv.Optional(CONF_RMS): NUM_SCHEMA,
            cv.Optional(CONF_ANOMALY_SCORE): NUM_SCHEMA,
            cv.Optional(CONF_ANOMALY_BAND): NUM_SCHEMA,
        }
    ).extend(cv.polling_component_schema("1s"))
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    cg.add(var.set_pins(config[CONF_BCLK_PIN], config[CONF_WS_PIN],
                        config[CONF_DIN_PIN]))
    cg.add(var.set_sample_rate(config[CONF_SAMPLE_RATE]))

    for k, setter in [
        (CONF_DBA,           "set_dba"),
        (CONF_RMS,           "set_rms"),
        (CONF_ANOMALY_SCORE, "set_anomaly_score"),
        (CONF_ANOMALY_BAND,  "set_anomaly_band"),
    ]:
        if k in config:
            s = await sensor.new_sensor(config[k])
            cg.add(getattr(var, setter)(s))
