// ============================================================================
// input_shaper_capture.cpp
//
// Streams raw BMI270 accelerometer samples over USB CDC as CSV, intended for
// Klipper input-shaper tuning.
//
// Hardware: ESP32-C3-MINI-1-N4 custom PCB ("PrinterMonitor")
//   - BMI270 on I2C (SDA=GPIO1, SCL=GPIO0, @0x68)  -- needs INPUT_PULLUP
//   - WS2812B status LED on GPIO5
//   - Slide switch SW3 on GPIO2 (start/stop capture)
//
// Protocol on Serial (USB CDC, baud is ignored on C3 native USB):
//   Header (once at boot):
//       # BMI270 input-shaper capture
//       # range_g=<g>  odr_hz=<odr>  scale_g_per_lsb=<k>
//       t_us,ax_g,ay_g,az_g
//   Data lines while SW3 is HIGH:
//       <t_us>,<ax_g>,<ay_g>,<az_g>
//   Marker lines:
//       # START
//       # STOP  samples=<n>  duration_s=<t>  eff_rate_hz=<r>
//
// LED states:
//   RED  - idle / IMU error
//   GREEN- capturing
//   BLUE - boot / waiting for IMU
// ============================================================================

#include <Arduino.h>
#include <Wire.h>
#include <FastLED.h>
#include "SparkFun_BMI270_Arduino_Library.h"

// -------- Board pins --------
#define PIN_SDA        1
#define PIN_SCL        0
#define PIN_LED        5
#define PIN_SW3        2   // capture start/stop

// -------- Config --------
static const uint8_t  BMI270_I2C_ADDR = 0x68;
// Accel range: smaller = more resolution, larger = won't clip on big bumps.
// ±4 g is a good default for a 3D-printer toolhead.
static const uint8_t  ACCEL_RANGE_G   = 4;
// 1600 Hz is the BMI270's max accel ODR; plenty for printer resonance (<80 Hz).
static const float    ACCEL_ODR_HZ    = 1600.0f;

// -------- State --------
BMI270 imu;
CRGB   leds[1];

static inline void setLed(const CRGB &c) {
    leds[0] = c;
    FastLED.show();
}

static bool initIMU() {
    // I2C with internal pull-ups (board has weak/absent externals).
    pinMode(PIN_SDA, INPUT_PULLUP);
    pinMode(PIN_SCL, INPUT_PULLUP);
    Wire.begin(PIN_SDA, PIN_SCL, 400000);
    delay(100);

    if (imu.beginI2C(BMI270_I2C_ADDR) != BMI2_OK) {
        return false;
    }

    // Configure accelerometer: 1600 Hz ODR, ±4 g range, performance mode.
    bmi2_sens_config cfg;
    cfg.type                 = BMI2_ACCEL;
    cfg.cfg.acc.odr          = BMI2_ACC_ODR_1600HZ;
    cfg.cfg.acc.range        = BMI2_ACC_RANGE_4G;
    cfg.cfg.acc.bwp          = BMI2_ACC_NORMAL_AVG4;
    cfg.cfg.acc.filter_perf  = BMI2_PERF_OPT_MODE;
    if (imu.setConfig(cfg) != BMI2_OK) return false;
    return true;
}

void setup() {
    Serial.begin(115200);

    FastLED.addLeds<WS2812B, PIN_LED, GRB>(leds, 1);
    FastLED.setBrightness(40);
    setLed(CRGB::Blue);

    pinMode(PIN_SW3, INPUT_PULLDOWN);

    // Give USB CDC a moment to come up so we don't lose the header.
    uint32_t t0 = millis();
    while (!Serial && millis() - t0 < 2000) { delay(10); }

    if (!initIMU()) {
        setLed(CRGB::Red);
        Serial.println("# ERROR: BMI270 init failed");
        while (true) { delay(1000); }
    }

    Serial.println("# BMI270 input-shaper capture");
    Serial.printf("# range_g=%u  odr_hz=%.0f  scale_g_per_lsb=%.8f\n",
                  ACCEL_RANGE_G, ACCEL_ODR_HZ,
                  (float)ACCEL_RANGE_G / 32768.0f);
    Serial.println("# Flip SW3 HIGH to start capture, LOW to stop.");
    Serial.println("t_us,ax_g,ay_g,az_g");

    setLed(CRGB::Red);  // idle
}

void loop() {
    // Wait (idle) until SW3 goes HIGH.
    while (digitalRead(PIN_SW3) == LOW) {
        delay(1);
    }

    // ---- Capture session ----
    setLed(CRGB::Green);
    Serial.println("# START");

    uint32_t  t_start_us = micros();
    uint32_t  n_samples  = 0;

    while (digitalRead(PIN_SW3) == HIGH) {
        if (imu.getSensorData() == BMI2_OK) {
            uint32_t t_us = micros() - t_start_us;
            // Library already converts raw to g using configured range.
            Serial.printf("%lu,%.5f,%.5f,%.5f\n",
                          (unsigned long)t_us,
                          imu.data.accelX,
                          imu.data.accelY,
                          imu.data.accelZ);
            n_samples++;
        }
        // No delay: tight loop polls the IMU. At 1600 Hz ODR, getSensorData
        // returns fresh data every ~625 us; Serial writes buffer to USB.
    }

    uint32_t elapsed_us = micros() - t_start_us;
    float    dur_s     = elapsed_us / 1e6f;
    float    eff_hz    = dur_s > 0 ? (n_samples / dur_s) : 0.0f;
    Serial.printf("# STOP  samples=%lu  duration_s=%.3f  eff_rate_hz=%.1f\n",
                  (unsigned long)n_samples, dur_s, eff_hz);

    setLed(CRGB::Red);  // back to idle

    // Debounce switch
    delay(200);
}
