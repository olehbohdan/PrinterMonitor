#pragma once

#include "esphome/core/component.h"
#include "esphome/components/i2c/i2c.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/text_sensor/text_sensor.h"

#include <vector>
#include <string>

#ifdef USE_ARDUINO
extern "C" {
#include "bmi270_api/bmi270.h"
#include "bmi270_api/bmi2.h"
#include "bmi270_api/bmi2_defs.h"
}
#endif

namespace esphome {
namespace bmi270 {

// 4096 samples * 6 bytes = 24 KB on the heap during a capture.
static constexpr int CAPTURE_N = 4096;
static constexpr int FFT_LOG2  = 12;
static constexpr int PSD_BINS  = 128;

class BMI270Component : public PollingComponent, public i2c::I2CDevice {
 public:
  void setup() override;
  void update() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

  void set_accel_x(sensor::Sensor *s) { accel_x_ = s; }
  void set_accel_y(sensor::Sensor *s) { accel_y_ = s; }
  void set_accel_z(sensor::Sensor *s) { accel_z_ = s; }
  void set_gyro_x(sensor::Sensor *s)  { gyro_x_ = s; }
  void set_gyro_y(sensor::Sensor *s)  { gyro_y_ = s; }
  void set_gyro_z(sensor::Sensor *s)  { gyro_z_ = s; }
  void set_vibration(sensor::Sensor *s) { vibration_ = s; }
  void set_peak_freq(sensor::Sensor *s) { peak_freq_ = s; }
  void set_capture_progress(sensor::Sensor *s) { capture_progress_ = s; }
  void set_capture_state(text_sensor::TextSensor *s) { capture_state_ = s; }
  void set_capture_axis_sensor(text_sensor::TextSensor *s) { capture_axis_sensor_ = s; }

  void start_capture(const std::string &axis);
  void abort_capture();
  bool is_capturing() const { return phase_ == PHASE_CAPTURING; }

  bool get_last_psd(std::vector<float> *out_freqs,
                    std::vector<float> *out_psd,
                    std::string *out_axis,
                    float *out_fs,
                    float *out_peak_hz);

  // Per-physical-axis PSD (idx 0=sensorX, 1=sensorY, 2=sensorZ).
  // Returns false if no capture has completed or idx out of range.
  bool get_psd_axis(int idx,
                    std::vector<float> *out_freqs,
                    std::vector<float> *out_psd,
                    std::string *out_axis,
                    float *out_fs,
                    float *out_peak_hz);

 protected:
#ifdef USE_ARDUINO
  struct bmi2_dev dev_{};
  bool dev_initialised_{false};
  bool try_begin_();
  int8_t configure_(uint8_t accel_odr);
  bool read_accel_gyro_();
  // Static C-API callbacks - intf_ptr is `this`.
  static int8_t bmi_read_cb_(uint8_t reg, uint8_t *data, uint32_t len, void *intf_ptr);
  static int8_t bmi_write_cb_(uint8_t reg, const uint8_t *data, uint32_t len, void *intf_ptr);
  static void bmi_delay_us_cb_(uint32_t period_us, void *intf_ptr);
  // Most recent sample (g and dps), populated by read_accel_gyro_().
  float ax_g_{0}, ay_g_{0}, az_g_{0};
  float gx_dps_{0}, gy_dps_{0}, gz_dps_{0};
  float lsb_to_g_{1.0f / 4096.0f};       // 8g range default
  float lsb_to_dps_{2000.0f / 32768.0f}; // 2000 dps default
  int8_t last_begin_rc_{0};
  uint32_t next_retry_ms_{0};
#endif
  bool ready_{false};

  // Vibration RMS window (existing)
  static constexpr size_t WINDOW_ = 50;
  float win_x_[WINDOW_]{}, win_y_[WINDOW_]{}, win_z_[WINDOW_]{};
  size_t win_idx_{0};
  bool win_full_{false};
  float bias_x_{0.0f}, bias_y_{0.0f}, bias_z_{1.0f};

  sensor::Sensor *accel_x_{nullptr}, *accel_y_{nullptr}, *accel_z_{nullptr};
  sensor::Sensor *gyro_x_{nullptr},  *gyro_y_{nullptr},  *gyro_z_{nullptr};
  sensor::Sensor *vibration_{nullptr};
  sensor::Sensor *peak_freq_{nullptr};
  sensor::Sensor *capture_progress_{nullptr};
  text_sensor::TextSensor *capture_state_{nullptr};
  text_sensor::TextSensor *capture_axis_sensor_{nullptr};

  // Capture state machine
  enum Phase { PHASE_IDLE, PHASE_CAPTURING, PHASE_ANALYZING, PHASE_DONE } phase_{PHASE_IDLE};
  std::string capture_axis_str_{"?"};
  uint32_t capture_started_ms_{0};
  int16_t *cap_x_{nullptr};
  int16_t *cap_y_{nullptr};
  int16_t *cap_z_{nullptr};
  int      cap_n_{0};
  uint32_t cap_first_us_{0};
  uint32_t cap_last_us_{0};
  uint32_t cap_next_sample_us_{0};
  bool     was_high_rate_{false};
  float    cap_lsb_per_g_{8192.0f};  // ±4g range

  std::vector<float> last_freqs_;
  std::vector<float> last_psd_;
  float    last_fs_{0.0f};
  float    last_peak_hz_{0.0f};
  std::string last_axis_;

  // Per-physical-axis results (sensorX, sensorY, sensorZ).
  std::vector<float> psd_axis_[3];
  float    peak_hz_axis_[3]{0.0f, 0.0f, 0.0f};

  void set_phase_(Phase p, const char *label);
  void configure_high_rate_();
  void configure_normal_rate_();
  void capture_step_();
  void analyze_();
  void cleanup_buffers_();

  // -------- USB-Serial-JTAG raw streaming (for Klipper-side shaper tuning) --
  // The Pi opens /dev/ttyACM* and sends commands; firmware streams raw
  // accel samples back at the BMI270's exact 1.6 kHz ODR.  This bypasses
  // the wifi/HA event path entirely so timing is deterministic.
  // Protocol (text in, binary out):
  //   "START\n"  -> switch to 1600 Hz, begin streaming 8-byte frames.
  //                 Frame: 0xAA 0x55 ax_lo ax_hi ay_lo ay_hi az_lo az_hi
  //                 (little-endian int16, raw LSBs at ±8g range).
  //   "STOP\n"   -> stop streaming, return to 100 Hz idle.
  //   "PING\n"   -> reply "PONG\n" (host probe).
  void usb_init_();
  void usb_service_();
  void usb_run_blocking_();
  bool usb_streaming_{false};
  uint32_t usb_stream_next_us_{0};
  char usb_rx_buf_[16]{};
  uint8_t usb_rx_len_{0};
};

}  // namespace bmi270
}  // namespace esphome
