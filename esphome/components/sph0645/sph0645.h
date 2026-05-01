#pragma once

#include "esphome/core/component.h"
#include "esphome/components/sensor/sensor.h"

#ifdef USE_ARDUINO
#include <Arduino.h>
#include <driver/i2s.h>
#endif

namespace esphome {
namespace sph0645 {

// Number of log-spaced "bands" we track for anomaly detection.
// 16 bands * 4 bytes/float * 2 (mean+M2) = ~128 bytes - fits easily.
static constexpr int N_BANDS = 16;
static constexpr int FFT_N   = 512;   // small FFT, runs fast on C3
static constexpr int FFT_LOG2 = 9;

class SPH0645Component : public PollingComponent {
 public:
  void setup() override;
  void update() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::LATE; }

  void set_pins(uint8_t bclk, uint8_t ws, uint8_t din) {
    bclk_pin_ = bclk; ws_pin_ = ws; din_pin_ = din;
  }
  void set_sample_rate(uint32_t r) { sample_rate_ = r; }
  void set_dba(sensor::Sensor *s) { dba_ = s; }
  void set_rms(sensor::Sensor *s) { rms_ = s; }
  void set_anomaly_score(sensor::Sensor *s) { anomaly_score_ = s; }
  void set_anomaly_band(sensor::Sensor *s) { anomaly_band_ = s; }

  // External control: HA toggles "learning" on print start.
  void start_baseline_learn();
  void freeze_baseline();

 protected:
  uint8_t bclk_pin_{10};
  uint8_t ws_pin_{3};
  uint8_t din_pin_{7};
  uint32_t sample_rate_{16000};
  bool i2s_ok_{false};

  sensor::Sensor *dba_{nullptr};
  sensor::Sensor *rms_{nullptr};
  sensor::Sensor *anomaly_score_{nullptr};
  sensor::Sensor *anomaly_band_{nullptr};

  // Streaming RMS accumulator, integrated over update_interval.
  double rms_sum_{0.0};
  uint32_t rms_n_{0};

  // Welford running mean & M2 (variance) per band.
  float band_mean_[N_BANDS]{};
  float band_m2_[N_BANDS]{};
  uint32_t band_n_{0};

  bool learning_{true};       // collecting baseline stats
  uint32_t learn_started_ms_{0};
  static constexpr uint32_t LEARN_MS = 5UL * 60UL * 1000UL;  // 5 min

  // Latest published anomaly bookkeeping
  float last_score_{0.0f};
  int   last_band_{-1};

  // Working buffers (heap-allocated in setup).
  int32_t *raw_buf_{nullptr};
  float   *fft_re_{nullptr};
  float   *fft_im_{nullptr};

  void process_frame_();
};

}  // namespace sph0645
}  // namespace esphome
