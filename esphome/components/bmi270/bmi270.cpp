#include "bmi270.h"
#include "esphome/core/log.h"
#include "esphome/core/hal.h"
#include "esphome/core/application.h"
#include <cmath>
#include <cstdlib>
#include <cstring>

#ifdef USE_ARDUINO
#include "driver/usb_serial_jtag.h"
#endif

namespace esphome {
namespace bmi270 {

static const char *const TAG = "bmi270";

void BMI270Component::setup() {
#ifdef USE_ARDUINO
  // Initialise our bmi2_dev struct (we own it; we never use SparkFun's
  // wrapper class because that one talks to Arduino's `Wire`, which is
  // never brought up on ESP32 - ESPHome uses the ESP-IDF i2c driver).
  dev_.intf = BMI2_I2C_INTF;
  dev_.intf_ptr = this;
  dev_.read = &BMI270Component::bmi_read_cb_;
  dev_.write = &BMI270Component::bmi_write_cb_;
  dev_.delay_us = &BMI270Component::bmi_delay_us_cb_;
  dev_.read_write_len = 32;  // 32-byte chunks, like SparkFun
  dev_.dummy_byte = 0;       // I2C: no dummy byte

  if (try_begin_()) {
    if (configure_(BMI2_ACC_ODR_100HZ) == BMI2_OK) {
      ready_ = true;
      ESP_LOGI(TAG, "BMI270 initialised (addr=0x%02X)", this->address_);
      usb_init_();
      return;
    }
  }
  ESP_LOGW(TAG, "BMI270 init failed at boot (last_rc=%d, addr=0x%02X). Will retry from loop().",
           last_begin_rc_, this->address_);
  next_retry_ms_ = millis() + 1000;
#else
  this->mark_failed();
#endif
}

#ifdef USE_ARDUINO
int8_t BMI270Component::bmi_read_cb_(uint8_t reg, uint8_t *data, uint32_t len, void *intf_ptr) {
  auto *self = static_cast<BMI270Component *>(intf_ptr);
  auto rc = self->read_register(reg, data, len);
  return rc == i2c::ERROR_OK ? BMI2_OK : BMI2_E_COM_FAIL;
}
int8_t BMI270Component::bmi_write_cb_(uint8_t reg, const uint8_t *data, uint32_t len, void *intf_ptr) {
  auto *self = static_cast<BMI270Component *>(intf_ptr);
  auto rc = self->write_register(reg, data, len);
  return rc == i2c::ERROR_OK ? BMI2_OK : BMI2_E_COM_FAIL;
}
void BMI270Component::bmi_delay_us_cb_(uint32_t period_us, void *intf_ptr) {
  delayMicroseconds(period_us);
}

bool BMI270Component::try_begin_() {
  for (int attempt = 1; attempt <= 3; attempt++) {
    int8_t rc = bmi270_init(&dev_);
    last_begin_rc_ = rc;
    if (rc == BMI2_OK) {
      dev_initialised_ = true;
      return true;
    }
    ESP_LOGW(TAG, "BMI270 bmi270_init attempt %d/3 rc=%d", attempt, rc);
    delay(150);
  }
  return false;
}

int8_t BMI270Component::configure_(uint8_t accel_odr) {
  uint8_t sens[2] = {BMI2_ACCEL, BMI2_GYRO};
  int8_t rc = bmi270_sensor_enable(sens, 2, &dev_);
  if (rc != BMI2_OK) {
    ESP_LOGE(TAG, "bmi270_sensor_enable rc=%d", rc);
    return rc;
  }

  bmi2_sens_config cfg[2] = {};
  cfg[0].type = BMI2_ACCEL;
  cfg[0].cfg.acc.odr = accel_odr;
  cfg[0].cfg.acc.bwp = BMI2_ACC_NORMAL_AVG4;
  cfg[0].cfg.acc.filter_perf = BMI2_PERF_OPT_MODE;
  cfg[0].cfg.acc.range = BMI2_ACC_RANGE_8G;

  cfg[1].type = BMI2_GYRO;
  cfg[1].cfg.gyr.odr = BMI2_GYR_ODR_100HZ;
  cfg[1].cfg.gyr.bwp = BMI2_GYR_NORMAL_MODE;
  cfg[1].cfg.gyr.filter_perf = BMI2_PERF_OPT_MODE;
  cfg[1].cfg.gyr.noise_perf = BMI2_POWER_OPT_MODE;
  cfg[1].cfg.gyr.range = BMI2_GYR_RANGE_2000;
  cfg[1].cfg.gyr.ois_range = BMI2_GYR_OIS_2000;

  rc = bmi270_set_sensor_config(cfg, 2, &dev_);
  if (rc != BMI2_OK) {
    ESP_LOGE(TAG, "bmi270_set_sensor_config rc=%d", rc);
    return rc;
  }
  // Update scaling for current ranges (8g, 2000 dps).
  lsb_to_g_ = 8.0f / 32768.0f;
  lsb_to_dps_ = 2000.0f / 32768.0f;
  cap_lsb_per_g_ = 32768.0f / 8.0f;  // ~4096 LSB/g at ±8g
  return BMI2_OK;
}

bool BMI270Component::read_accel_gyro_() {
  uint8_t buf[12];
  if (bmi2_get_regs(BMI2_ACC_X_LSB_ADDR, buf, 12, &dev_) != BMI2_OK) return false;
  int16_t ax = (int16_t)((uint16_t) buf[0]  | ((uint16_t) buf[1]  << 8));
  int16_t ay = (int16_t)((uint16_t) buf[2]  | ((uint16_t) buf[3]  << 8));
  int16_t az = (int16_t)((uint16_t) buf[4]  | ((uint16_t) buf[5]  << 8));
  int16_t gx = (int16_t)((uint16_t) buf[6]  | ((uint16_t) buf[7]  << 8));
  int16_t gy = (int16_t)((uint16_t) buf[8]  | ((uint16_t) buf[9]  << 8));
  int16_t gz = (int16_t)((uint16_t) buf[10] | ((uint16_t) buf[11] << 8));
  ax_g_ = ax * lsb_to_g_;
  ay_g_ = ay * lsb_to_g_;
  az_g_ = az * lsb_to_g_;
  gx_dps_ = gx * lsb_to_dps_;
  gy_dps_ = gy * lsb_to_dps_;
  gz_dps_ = gz * lsb_to_dps_;
  return true;
}
#endif



void BMI270Component::configure_normal_rate_() {
#ifdef USE_ARDUINO
  configure_(BMI2_ACC_ODR_100HZ);
#endif
}

void BMI270Component::configure_high_rate_() {
#ifdef USE_ARDUINO
  configure_(BMI2_ACC_ODR_1600HZ);
  delay(2);
#endif
}

void BMI270Component::dump_config() {
  ESP_LOGCONFIG(TAG, "BMI270:");
  LOG_I2C_DEVICE(this);
  LOG_UPDATE_INTERVAL(this);
  ESP_LOGCONFIG(TAG, "  ready=%s captureN=%d psdBins=%d", ready_ ? "y" : "n", CAPTURE_N, PSD_BINS);
}

void BMI270Component::update() {
#ifdef USE_ARDUINO
  if (!ready_) return;
  if (phase_ == PHASE_CAPTURING || phase_ == PHASE_ANALYZING) return;
  if (!read_accel_gyro_()) return;

  const float ax = ax_g_;  // already in g
  const float ay = ay_g_;
  const float az = az_g_;

  // Accel sensors output m/s^2 to keep the existing YAML schema happy.
  if (accel_x_) accel_x_->publish_state(ax * 9.80665f);
  if (accel_y_) accel_y_->publish_state(ay * 9.80665f);
  if (accel_z_) accel_z_->publish_state(az * 9.80665f);
  if (gyro_x_)  gyro_x_->publish_state(gx_dps_);
  if (gyro_y_)  gyro_y_->publish_state(gy_dps_);
  if (gyro_z_)  gyro_z_->publish_state(gz_dps_);

  // Vibration RMS - high-pass via slow EMA bias removal
  const float a = 0.02f;
  bias_x_ = (1 - a) * bias_x_ + a * ax;
  bias_y_ = (1 - a) * bias_y_ + a * ay;
  bias_z_ = (1 - a) * bias_z_ + a * az;
  float dx = ax - bias_x_, dy = ay - bias_y_, dz = az - bias_z_;
  win_x_[win_idx_] = dx * dx;
  win_y_[win_idx_] = dy * dy;
  win_z_[win_idx_] = dz * dz;
  win_idx_ = (win_idx_ + 1) % WINDOW_;
  if (win_idx_ == 0) win_full_ = true;
  if (vibration_) {
    size_t n = win_full_ ? WINDOW_ : win_idx_;
    if (n) {
      float s = 0;
      for (size_t i = 0; i < n; i++) s += win_x_[i] + win_y_[i] + win_z_[i];
      vibration_->publish_state(std::sqrt(s / n));
    }
  }
#endif
}

void BMI270Component::loop() {
#ifdef USE_ARDUINO
  // Background re-init if setup() failed. Retries every ~5s, with the rc
  // code visible in HA/OTA logs.
  if (!ready_ && next_retry_ms_ != 0 && (int32_t)(millis() - next_retry_ms_) >= 0) {
    if (try_begin_() && configure_(BMI2_ACC_ODR_100HZ) == BMI2_OK) {
      ready_ = true;
      next_retry_ms_ = 0;
      ESP_LOGI(TAG, "BMI270 initialised on retry (addr=0x%02X)", this->address_);
      usb_init_();
    } else {
      ESP_LOGE(TAG, "BMI270 init still failing rc=%d (addr=0x%02X)",
               last_begin_rc_, this->address_);
      next_retry_ms_ = millis() + 5000;
    }
  }
  if (phase_ != PHASE_CAPTURING) {
    usb_service_();
    return;
  }
  capture_step_();
#endif
}

void BMI270Component::set_phase_(Phase p, const char *label) {
  phase_ = p;
  if (capture_state_) capture_state_->publish_state(label);
  ESP_LOGI(TAG, "phase -> %s", label);
}

void BMI270Component::start_capture(const std::string &axis) {
#ifdef USE_ARDUINO
  if (!ready_) { ESP_LOGW(TAG, "not ready"); return; }
  if (phase_ == PHASE_CAPTURING || phase_ == PHASE_ANALYZING) {
    ESP_LOGW(TAG, "capture in progress"); return;
  }
  cleanup_buffers_();
  cap_x_ = (int16_t *) malloc(CAPTURE_N * sizeof(int16_t));
  cap_y_ = (int16_t *) malloc(CAPTURE_N * sizeof(int16_t));
  cap_z_ = (int16_t *) malloc(CAPTURE_N * sizeof(int16_t));
  if (!cap_x_ || !cap_y_ || !cap_z_) {
    ESP_LOGE(TAG, "capture alloc fail");
    cleanup_buffers_();
    return;
  }
  cap_n_ = 0;
  cap_first_us_ = cap_last_us_ = 0;
  capture_axis_str_ = axis;
  if (capture_axis_sensor_) capture_axis_sensor_->publish_state(axis);
  capture_started_ms_ = millis();
  configure_high_rate_();
  was_high_rate_ = true;
  set_phase_(PHASE_CAPTURING, "capturing");
#endif
}

void BMI270Component::abort_capture() {
#ifdef USE_ARDUINO
  if (was_high_rate_) { configure_normal_rate_(); was_high_rate_ = false; }
  cleanup_buffers_();
  set_phase_(PHASE_IDLE, "idle");
#endif
}

void BMI270Component::cleanup_buffers_() {
  if (cap_x_) { free(cap_x_); cap_x_ = nullptr; }
  if (cap_y_) { free(cap_y_); cap_y_ = nullptr; }
  if (cap_z_) { free(cap_z_); cap_z_ = nullptr; }
  cap_n_ = 0;
}

void BMI270Component::capture_step_() {
#ifdef USE_ARDUINO
  // Single-shot blocking capture.  At 1600 Hz ODR we need to hit the chip
  // every 625 us; ESPHome's loop() only runs every ~16 ms, so any attempt
  // at cooperative scheduling (DRDY-poll or time-gated bursts that yield
  // back to loop()) ends up bursty at a few hundred sps.  Instead we just
  // block here for the entire 4096-sample capture (~2.56 s).  ESPHome
  // logs a "took too long" warning but the WiFi / API stack survives;
  // capture only runs once per shaper sweep so the impact is negligible.
  const uint32_t sample_period_us = 625;       // 1/1600 Hz
  const uint32_t hard_timeout_ms = 8000;        // safety
  const uint32_t start_ms = millis();
  uint32_t next_us = micros();

  while (cap_n_ < CAPTURE_N) {
    // Wait until the next scheduled sample tick (or just continue if behind).
    while ((int32_t)(next_us - micros()) > 0) { /* spin */ }

    uint8_t buf[6];
    if (bmi2_get_regs(BMI2_ACC_X_LSB_ADDR, buf, 6, &dev_) != BMI2_OK) break;
    int16_t ix = (int16_t)((uint16_t) buf[0] | ((uint16_t) buf[1] << 8));
    int16_t iy = (int16_t)((uint16_t) buf[2] | ((uint16_t) buf[3] << 8));
    int16_t iz = (int16_t)((uint16_t) buf[4] | ((uint16_t) buf[5] << 8));

    uint32_t t = micros();
    if (cap_n_ == 0) cap_first_us_ = t;
    cap_last_us_ = t;
    cap_x_[cap_n_] = ix;
    cap_y_[cap_n_] = iy;
    cap_z_[cap_n_] = iz;
    cap_n_++;
    next_us += sample_period_us;

    // If the read itself took longer than the period (slow I2C glitch),
    // resync to "now" so we don't burst-read duplicates trying to catch up.
    if ((int32_t)(micros() - next_us) > 2000) next_us = micros();

    // Periodic feed of WiFi/RTOS so the OTA / API socket doesn't reset.
    if ((cap_n_ & 0x1FF) == 0) {
      yield();
      if (millis() - start_ms > hard_timeout_ms) break;
    }
  }

  if (capture_progress_)
    capture_progress_->publish_state(100.0f * cap_n_ / CAPTURE_N);

  // Always advance to analyze - we either filled the buffer or hit timeout.
  {
    set_phase_(PHASE_ANALYZING, "analyzing");
    if (was_high_rate_) { configure_normal_rate_(); was_high_rate_ = false; }
    analyze_();
    cleanup_buffers_();
    set_phase_(PHASE_DONE, "done");
  }
#endif
}

// In-place radix-2 FFT (Cooley-Tukey)
static void fft_radix2(float *re, float *im, int log2n) {
  int n = 1 << log2n;
  int j = 0;
  for (int i = 1; i < n; i++) {
    int bit = n >> 1;
    for (; j & bit; bit >>= 1) j ^= bit;
    j ^= bit;
    if (i < j) {
      float tr = re[i]; re[i] = re[j]; re[j] = tr;
      float ti = im[i]; im[i] = im[j]; im[j] = ti;
    }
  }
  for (int s = 1; s <= log2n; s++) {
    int m = 1 << s;
    int m2 = m >> 1;
    float theta = -3.14159265358979f / m2;
    float wr_step = cosf(theta), wi_step = sinf(theta);
    for (int k = 0; k < n; k += m) {
      float wr = 1.0f, wi = 0.0f;
      for (int i = 0; i < m2; i++) {
        float tr = wr * re[k + i + m2] - wi * im[k + i + m2];
        float ti = wr * im[k + i + m2] + wi * re[k + i + m2];
        re[k + i + m2] = re[k + i] - tr;
        im[k + i + m2] = im[k + i] - ti;
        re[k + i] += tr;
        im[k + i] += ti;
        float nwr = wr * wr_step - wi * wi_step;
        wi = wr * wi_step + wi * wr_step;
        wr = nwr;
      }
    }
  }
}

void BMI270Component::analyze_() {
#ifdef USE_ARDUINO
  if (cap_n_ < 256 || cap_last_us_ <= cap_first_us_) {
    ESP_LOGW(TAG, "analyze: too few samples (%d)", cap_n_);
    return;
  }
  const int N = cap_n_;
  const int FFT_N = 1 << FFT_LOG2;
  const float dt_s = (cap_last_us_ - cap_first_us_) * 1e-6f;
  const float fs = (N - 1) / dt_s;
  ESP_LOGI(TAG, "analyze: N=%d fs=%.1f Hz", N, fs);

  static float re[1 << FFT_LOG2];
  static float im[1 << FFT_LOG2];
  static float win[1 << FFT_LOG2];
  static bool win_init = false;
  if (!win_init) {
    for (int i = 0; i < FFT_N; i++)
      win[i] = 0.5f - 0.5f * cosf(2.0f * 3.14159265f * i / (FFT_N - 1));
    win_init = true;
  }

  float best_peak_pow = 0.0f;
  int   best_axis_id = 0;
  float best_peak_hz = 0.0f;
  std::vector<float> best_psd;

  // Reset per-axis storage so stale data doesn't survive a partial capture.
  for (int i = 0; i < 3; i++) {
    psd_axis_[i].clear();
    peak_hz_axis_[i] = 0.0f;
  }

  for (int axis_id = 0; axis_id < 3; axis_id++) {
    int16_t *src = (axis_id == 0) ? cap_x_ : (axis_id == 1) ? cap_y_ : cap_z_;
    double mean = 0;
    for (int i = 0; i < N; i++) mean += (double) src[i];
    mean /= N;
    int M = (N < FFT_N) ? N : FFT_N;
    for (int i = 0; i < M; i++) {
      float v = (src[i] - mean) / cap_lsb_per_g_;
      re[i] = v * win[i];
      im[i] = 0;
    }
    for (int i = M; i < FFT_N; i++) { re[i] = 0; im[i] = 0; }

    fft_radix2(re, im, FFT_LOG2);

    std::vector<float> psd(PSD_BINS, 0.0f);
    std::vector<int>   cnt(PSD_BINS, 0);
    const int half = FFT_N / 2;
    const float bin_to_freq = fs / FFT_N;
    const float fmax = fs * 0.5f;
    const float bin_step = fmax / PSD_BINS;
    float peak_pow = 0.0f;
    int   peak_idx = -1;
    for (int k = 1; k < half; k++) {
      float f = k * bin_to_freq;
      if (f >= fmax) break;
      float p = re[k] * re[k] + im[k] * im[k];
      int b = (int) (f / bin_step);
      if (b >= PSD_BINS) b = PSD_BINS - 1;
      psd[b] += p;
      cnt[b]++;
      if (f >= 5.0f && f <= 120.0f && p > peak_pow) {
        peak_pow = p;
        peak_idx = k;
      }
    }
    for (int i = 0; i < PSD_BINS; i++)
      if (cnt[i]) psd[i] /= cnt[i];

    // Per-axis peak (in the 5-120 Hz band).
    float axis_peak_hz = (peak_idx >= 0) ? peak_idx * bin_to_freq : 0.0f;
    peak_hz_axis_[axis_id] = axis_peak_hz;
    psd_axis_[axis_id] = psd;  // copy; we still need a copy to move into best_psd below

    if (peak_pow > best_peak_pow) {
      best_peak_pow = peak_pow;
      best_axis_id = axis_id;
      best_psd = std::move(psd);
      best_peak_hz = axis_peak_hz;
    }
  }

  last_freqs_.resize(PSD_BINS);
  const float bin_step = (fs * 0.5f) / PSD_BINS;
  for (int i = 0; i < PSD_BINS; i++) last_freqs_[i] = (i + 0.5f) * bin_step;
  last_psd_ = std::move(best_psd);
  last_fs_ = fs;
  last_peak_hz_ = best_peak_hz;
  const char *axn = (best_axis_id == 0) ? "X" : (best_axis_id == 1) ? "Y" : "Z";
  last_axis_ = capture_axis_str_ + std::string("/sensor-") + axn;
  if (peak_freq_) peak_freq_->publish_state(last_peak_hz_);
  ESP_LOGI(TAG, "peak in sensor-%s axis: %.1f Hz (X=%.1f Y=%.1f Z=%.1f)", axn,
           last_peak_hz_, peak_hz_axis_[0], peak_hz_axis_[1], peak_hz_axis_[2]);
#endif
}

bool BMI270Component::get_psd_axis(int idx, std::vector<float> *of, std::vector<float> *op,
                                   std::string *oa, float *ofs, float *opk) {
  if (idx < 0 || idx > 2) return false;
  if (psd_axis_[idx].empty()) return false;
  if (of) *of = last_freqs_;
  if (op) *op = psd_axis_[idx];
  if (oa) {
    const char *axn = (idx == 0) ? "X" : (idx == 1) ? "Y" : "Z";
    *oa = capture_axis_str_ + std::string("/sensor-") + axn;
  }
  if (ofs) *ofs = last_fs_;
  if (opk) *opk = peak_hz_axis_[idx];
  return true;
}

bool BMI270Component::get_last_psd(std::vector<float> *of, std::vector<float> *op,
                                   std::string *oa, float *ofs, float *opk) {
  if (last_psd_.empty()) return false;
  if (of) *of = last_freqs_;
  if (op) *op = last_psd_;
  if (oa) *oa = last_axis_;
  if (ofs) *ofs = last_fs_;
  if (opk) *opk = last_peak_hz_;
  return true;
}

// ============================================================================
// USB-Serial-JTAG raw streaming
// ============================================================================
void BMI270Component::usb_init_() {
#ifdef USE_ARDUINO
  // 4 KB TX buffer is plenty for 8 B/sample * 1600 Hz = 12.8 KB/s with
  // ~250 ms host-side jitter tolerance.  RX only needs to hold short
  // text commands.
  usb_serial_jtag_driver_config_t cfg = {
      .tx_buffer_size = 4096,
      .rx_buffer_size = 256,
  };
  esp_err_t err = usb_serial_jtag_driver_install(&cfg);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "usb_serial_jtag_driver_install failed (%d) - "
                  "USB shaper streaming disabled", (int) err);
  } else {
    ESP_LOGI(TAG, "USB-Serial-JTAG ready for shaper streaming");
  }
#endif
}

void BMI270Component::usb_service_() {
#ifdef USE_ARDUINO
  if (!ready_) return;

  // 1. Pull any pending command bytes (non-blocking).
  uint8_t rx[64];
  int n = usb_serial_jtag_read_bytes(rx, sizeof(rx), 0);
  for (int i = 0; i < n; i++) {
    char c = (char) rx[i];
    if (c == '\r') continue;
    if (c == '\n') {
      usb_rx_buf_[usb_rx_len_] = 0;
      if (strcmp(usb_rx_buf_, "START") == 0) {
        if (!usb_streaming_) {
          // Run the entire capture inline. This blocks loop() for up to
          // ~8 s, but the device is plugged into the Pi for tuning so
          // the display/wifi pause is fine. Returning to ESPHome after
          // STOP is essential for wifi to stay alive.
          usb_run_blocking_();
          // After return, the rest of the rx[] loop continues normally.
        }
      } else if (strcmp(usb_rx_buf_, "STOP") == 0) {
        // STOP arriving outside of a streaming run is a no-op; the
        // blocking streamer handles its own STOP detection.
      } else if (strcmp(usb_rx_buf_, "PING") == 0) {
        const char *ack = "PONG\n";
        usb_serial_jtag_write_bytes(ack, 5, 0);
      }
      usb_rx_len_ = 0;
    } else if (usb_rx_len_ < sizeof(usb_rx_buf_) - 1) {
      usb_rx_buf_[usb_rx_len_++] = c;
    } else {
      // Buffer overflow - reset to avoid getting stuck on garbage.
      usb_rx_len_ = 0;
    }
  }
#endif
}

// Run a blocking high-rate capture: switch BMI to 1600 Hz, ack OK\n,
// then push 8-byte frames at 625 us cadence until STOP arrives or the
// 8 s safety cap fires. Returns to caller in normal-rate mode.
void BMI270Component::usb_run_blocking_() {
#ifdef USE_ARDUINO
  configure_high_rate_();
  usb_streaming_ = true;
  const char *ack_ok = "OK\n";
  usb_serial_jtag_write_bytes(ack_ok, 3, 0);
  ESP_LOGI(TAG, "USB stream START (blocking)");

  const uint32_t period_us = 625;
  // Long enough to cover a full Klipper TEST_RESONANCES chirp (~30 s
  // at FREQ_START=20, FREQ_END=80, HZ_PER_SEC=2). The python script
  // sends STOP at the end of its capture window so we normally exit
  // well before this cap.
  const uint32_t safety_cap_us = 35000000;  // 35 s hard limit
  const uint32_t t_start = micros();
  uint32_t next_us = t_start;
  uint8_t frame[8];
  frame[0] = 0xAA;
  frame[1] = 0x55;

  // Local STOP-line buffer, reused without touching the class state.
  char stop_buf[8] = {0};
  uint8_t stop_len = 0;
  bool got_stop = false;
  uint32_t samples = 0;

  while (!got_stop && (micros() - t_start) < safety_cap_us) {
    // Spin until next tick (typical wait 0-625 us).
    while ((int32_t)(next_us - micros()) > 0) { /* tight spin */ }

    uint8_t buf[6];
    if (bmi2_get_regs(BMI2_ACC_X_LSB_ADDR, buf, 6, &dev_) != BMI2_OK) break;
    frame[2] = buf[0]; frame[3] = buf[1];
    frame[4] = buf[2]; frame[5] = buf[3];
    frame[6] = buf[4]; frame[7] = buf[5];
    usb_serial_jtag_write_bytes(frame, 8, 0);
    samples++;

    next_us += period_us;
    if ((int32_t)(micros() - next_us) > 2000)
      next_us = micros();  // resync after stall

    // Poll for STOP every 32 samples (~20 ms) - cheap.
    if ((samples & 0x1F) == 0) {
      uint8_t rx[16];
      int n = usb_serial_jtag_read_bytes(rx, sizeof(rx), 0);
      for (int i = 0; i < n; i++) {
        char c = (char) rx[i];
        if (c == '\r') continue;
        if (c == '\n') {
          stop_buf[stop_len] = 0;
          if (strcmp(stop_buf, "STOP") == 0) got_stop = true;
          stop_len = 0;
        } else if (stop_len < sizeof(stop_buf) - 1) {
          stop_buf[stop_len++] = c;
        } else {
          stop_len = 0;
        }
      }
      // Keep the watchdog and IDF tasks happy.
      App.feed_wdt();
    }
  }

  usb_streaming_ = false;
  configure_normal_rate_();
  const char *ack_stopped = "STOPPED\n";
  usb_serial_jtag_write_bytes(ack_stopped, 8, 0);
  uint32_t elapsed_us = micros() - t_start;
  ESP_LOGI(TAG, "USB stream STOP (samples=%u, elapsed=%u us, fs=%.0f)",
           (unsigned) samples, (unsigned) elapsed_us,
           samples * 1.0e6f / (elapsed_us > 0 ? elapsed_us : 1));
#endif
}

}  // namespace bmi270
}  // namespace esphome
