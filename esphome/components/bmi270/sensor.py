import esphome.codegen as cg
import esphome.config_validation as cv
from esphome import pins
from esphome.components import i2c, sensor
from esphome.const import (
    CONF_ID,
    CONF_ADDRESS,
    CONF_UPDATE_INTERVAL,
    UNIT_METER_PER_SECOND_SQUARED,
    ICON_VIBRATE,
    STATE_CLASS_MEASUREMENT,
)
CODEOWNERS = ["@local"]
DEPENDENCIES = ["i2c"]
AUTO_LOAD = ["sensor"]
MULTI_CONF = False

bmi270_ns = cg.esphome_ns.namespace("bmi270")
BMI270Component = bmi270_ns.class_(
    "BMI270Component", cg.PollingComponent, i2c.I2CDevice
)

CONF_ACCEL_X = "accel_x"
CONF_ACCEL_Y = "accel_y"
CONF_ACCEL_Z = "accel_z"
CONF_GYRO_X = "gyro_x"
CONF_GYRO_Y = "gyro_y"
CONF_GYRO_Z = "gyro_z"
CONF_VIBRATION = "vibration"
CONF_PEAK_FREQ = "peak_freq"
CONF_CAPTURE_PROGRESS = "capture_progress"

ACCEL_SCHEMA = sensor.sensor_schema(
    unit_of_measurement=UNIT_METER_PER_SECOND_SQUARED,
    accuracy_decimals=2,
    state_class=STATE_CLASS_MEASUREMENT,
)

GYRO_SCHEMA = sensor.sensor_schema(
    unit_of_measurement="°/s",
    accuracy_decimals=1,
    state_class=STATE_CLASS_MEASUREMENT,
)

VIB_SCHEMA = sensor.sensor_schema(
    unit_of_measurement="g",
    accuracy_decimals=3,
    icon=ICON_VIBRATE,
    state_class=STATE_CLASS_MEASUREMENT,
)

CONFIG_SCHEMA = (
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(BMI270Component),
            cv.Optional(CONF_ACCEL_X): ACCEL_SCHEMA,
            cv.Optional(CONF_ACCEL_Y): ACCEL_SCHEMA,
            cv.Optional(CONF_ACCEL_Z): ACCEL_SCHEMA,
            cv.Optional(CONF_GYRO_X): GYRO_SCHEMA,
            cv.Optional(CONF_GYRO_Y): GYRO_SCHEMA,
            cv.Optional(CONF_GYRO_Z): GYRO_SCHEMA,
            cv.Optional(CONF_VIBRATION): VIB_SCHEMA,
            cv.Optional(CONF_PEAK_FREQ): sensor.sensor_schema(
                unit_of_measurement="Hz",
                accuracy_decimals=2,
                state_class=STATE_CLASS_MEASUREMENT,
            ),
            cv.Optional(CONF_CAPTURE_PROGRESS): sensor.sensor_schema(
                unit_of_measurement="%",
                accuracy_decimals=0,
                state_class=STATE_CLASS_MEASUREMENT,
            ),
        }
    )
    .extend(cv.polling_component_schema("500ms"))
    .extend(i2c.i2c_device_schema(0x68))
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await i2c.register_i2c_device(var, config)


    if CONF_ACCEL_X in config:
        s = await sensor.new_sensor(config[CONF_ACCEL_X])
        cg.add(var.set_accel_x(s))
    if CONF_ACCEL_Y in config:
        s = await sensor.new_sensor(config[CONF_ACCEL_Y])
        cg.add(var.set_accel_y(s))
    if CONF_ACCEL_Z in config:
        s = await sensor.new_sensor(config[CONF_ACCEL_Z])
        cg.add(var.set_accel_z(s))
    if CONF_GYRO_X in config:
        s = await sensor.new_sensor(config[CONF_GYRO_X])
        cg.add(var.set_gyro_x(s))
    if CONF_GYRO_Y in config:
        s = await sensor.new_sensor(config[CONF_GYRO_Y])
        cg.add(var.set_gyro_y(s))
    if CONF_GYRO_Z in config:
        s = await sensor.new_sensor(config[CONF_GYRO_Z])
        cg.add(var.set_gyro_z(s))
    if CONF_VIBRATION in config:
        s = await sensor.new_sensor(config[CONF_VIBRATION])
        cg.add(var.set_vibration(s))
    if CONF_PEAK_FREQ in config:
        s = await sensor.new_sensor(config[CONF_PEAK_FREQ])
        cg.add(var.set_peak_freq(s))
    if CONF_CAPTURE_PROGRESS in config:
        s = await sensor.new_sensor(config[CONF_CAPTURE_PROGRESS])
        cg.add(var.set_capture_progress(s))

    # Wire/SPI are part of arduino-esp32 core but SparkFun's library.json
    # doesn't declare them as deps, so PlatformIO's library scanner misses
    # them. Declaring them here forces them onto the include path.
    cg.add_library("Wire", None)
    cg.add_library("SPI", None)

    # Use the SparkFun BMI270 Arduino library.
    cg.add_library(
        "SparkFun BMI270 Arduino Library",
        None,
        "https://github.com/sparkfun/SparkFun_BMI270_Arduino_Library.git",
    )
