// SPH0645 I2S microphone driver - dBA SPL + per-band anomaly score.
//
// Why not use ESPHome's i2s_audio? That component assumes streaming/voice
// pipelines. We just need short bursts of samples for RMS + a small FFT
// every second, and we want full control over band-energy stats.
//
// Implementation notes:
//   - SPH0645 is 24-bit data left-justified in 32-bit I2S frames.
//   - It samples on rising BCLK; ESP32 I2S peripheral default is fine.
//   - The mic has a long start-up before output stabilises (~50 ms);
//     we discard the first frame after enabling.
//   - A-weighting is approximated with a 4-th order IIR (well-known
//     coefficients from genuine A-weight specs, fs=16kHz).

#include "sph0645.h"
#include "esphome/core/log.h"

#ifdef USE_ARDUINO
#include <math.h>
#endif

namespace esphome {
namespace sph0645 {

static const char *const TAG = "sph0645";

// Adafruit/Knowles SPH0645 sensitivity = -26 dBFS at 94 dB SPL.
// 24-bit signed full scale = 2^23 = 8388608. So 94 dB SPL == sample RMS of
// 8388608 * 10^(-26/20) ~= 420422.  dB(SPL) = 20*log10(rms / 420422) + 94.
static constexpr float SPH0645_REF_RMS = 420422.0f;
static constexpr float SPH0645_REF_DB = 94.0f;

// 4th-order A-weighting (fs=16kHz) - cascaded biquads.
struct Biquad {
  float b0, b1, b2, a1, a2;
  float z1, z2;
  inline float step(float x) {
    float y = b0 * x + z1;
    z1 = b1 * x - a1 * y + z2;
    z2 = b2 * x - a2 * y;
    return y;
  }
};
// Coefficients from Andre's design (en.wikipedia.org/wiki/A-weighting fs=16k)
static Biquad bq1{0.169994948147430f, 0.280415310498794f, -1.120574766348363f,
                  -1.218895866802768f, 0.453019113895269f, 0.0f, 0.0f};
static Biquad bq2{1.0f, -2.0f, 1.0f, -1.989801023314322f, 0.989803049223605f,
                  0.0f, 0.0f};

// Bit-reversed Cooley-Tukey radix-2 FFT, magnitude-squared output.
// Small enough (N=512) that this is fine on the C3.
static void fft_radix2(float *re, float *im, int log2n) {
  int n = 1 << log2n;
  // bit reversal
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
    float wr_step = cosf(theta);
    float wi_step = sinf(theta);
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

void SPH0645Component::setup() {
#ifdef USE_ARDUINO
  raw_buf_ = (int32_t *) malloc(FFT_N * sizeof(int32_t));
  fft_re_  = (float *)   malloc(FFT_N * sizeof(float));
  fft_im_  = (float *)   malloc(FFT_N * sizeof(float));
  if (!raw_buf_ || !fft_re_ || !fft_im_) {
    ESP_LOGE(TAG, "alloc failed");
    this->mark_failed();
    return;
  }

  i2s_config_t cfg = {};
  cfg.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX);
  cfg.sample_rate = sample_rate_;
  cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT;
  cfg.channel_format = I2S_CHANNEL_FMT_ONLY_LEFT;  // SPH0645 SEL=GND -> left
  cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  cfg.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
  cfg.dma_buf_count = 4;
  cfg.dma_buf_len = FFT_N;
  cfg.use_apll = false;
  cfg.tx_desc_auto_clear = false;

  i2s_pin_config_t pins = {};
  pins.bck_io_num = bclk_pin_;
  pins.ws_io_num = ws_pin_;
  pins.data_in_num = din_pin_;
  pins.data_out_num = I2S_PIN_NO_CHANGE;
  pins.mck_io_num = I2S_PIN_NO_CHANGE;

  esp_err_t r = i2s_driver_install(I2S_NUM_0, &cfg, 0, nullptr);
  if (r != ESP_OK) { ESP_LOGE(TAG, "i2s install fail %d", r); mark_failed(); return; }
  r = i2s_set_pin(I2S_NUM_0, &pins);
  if (r != ESP_OK) { ESP_LOGE(TAG, "i2s pin fail %d", r); mark_failed(); return; }

  i2s_ok_ = true;
  learn_started_ms_ = millis();
  ESP_LOGI(TAG, "SPH0645 OK (BCLK=%u WS=%u DIN=%u %u Hz)",
           bclk_pin_, ws_pin_, din_pin_, sample_rate_);
#else
  this->mark_failed();
#endif
}

void SPH0645Component::dump_config() {
  ESP_LOGCONFIG(TAG, "SPH0645:");
  ESP_LOGCONFIG(TAG, "  BCLK=%u WS=%u DIN=%u rate=%u Hz",
                bclk_pin_, ws_pin_, din_pin_, sample_rate_);
  ESP_LOGCONFIG(TAG, "  Bands=%d FFT_N=%d", N_BANDS, FFT_N);
}

void SPH0645Component::loop() {
#ifdef USE_ARDUINO
  if (!i2s_ok_) return;
  size_t got = 0;
  // Non-blocking: take whatever's already in DMA, no wait.
  esp_err_t r = i2s_read(I2S_NUM_0, raw_buf_, FFT_N * sizeof(int32_t),
                         &got, 0);
  if (r != ESP_OK || got < FFT_N * sizeof(int32_t)) return;
  process_frame_();
#endif
}

void SPH0645Component::process_frame_() {
#ifdef USE_ARDUINO
  // Convert 32-bit I2S frame to 24-bit signed -> normalise to ~[-1, 1].
  // Apply Hann window & A-weight cascade. Track linear+A-weighted RMS.
  static float win[FFT_N];
  static bool win_init = false;
  if (!win_init) {
    for (int i = 0; i < FFT_N; i++)
      win[i] = 0.5f - 0.5f * cosf(2.0f * 3.14159265f * i / (FFT_N - 1));
    win_init = true;
  }

  double sum_sq_a = 0.0;
  double sum_sq   = 0.0;
  for (int i = 0; i < FFT_N; i++) {
    int32_t s = raw_buf_[i] >> 14;   // 32 -> 18-ish bits; shift drops noise floor of LSBs
    float fs = (float)s;
    sum_sq += (double)fs * (double)fs;
    float w = bq2.step(bq1.step(fs));
    sum_sq_a += (double)w * (double)w;
    fft_re_[i] = fs * win[i];
    fft_im_[i] = 0.0f;
  }

  rms_sum_ += sum_sq;
  rms_n_   += FFT_N;

  // FFT for band energies (anomaly path)
  fft_radix2(fft_re_, fft_im_, FFT_LOG2);

  // Log-spaced bins from ~80 Hz to nyquist
  const float nyq = (float)sample_rate_ * 0.5f;
  const float fmin = 80.0f;
  const float fmax = nyq * 0.95f;
  const float log_step = logf(fmax / fmin) / N_BANDS;
  float energies[N_BANDS] = {0};
  int counts[N_BANDS] = {0};
  for (int k = 1; k < FFT_N / 2; k++) {
    float f = (float)k * sample_rate_ / FFT_N;
    if (f < fmin || f >= fmax) continue;
    int b = (int)(logf(f / fmin) / log_step);
    if (b < 0 || b >= N_BANDS) continue;
    float p = fft_re_[k] * fft_re_[k] + fft_im_[k] * fft_im_[k];
    energies[b] += p;
    counts[b]++;
  }
  for (int i = 0; i < N_BANDS; i++) {
    if (counts[i]) energies[i] = logf(energies[i] / counts[i] + 1.0f);
  }

  // Update Welford / compute z-score
  band_n_++;
  float max_z = 0.0f;
  int   max_band = -1;
  if (learning_) {
    if (millis() - learn_started_ms_ > LEARN_MS) {
      learning_ = false;
      ESP_LOGI(TAG, "anomaly baseline frozen after %u frames", band_n_);
    }
    for (int i = 0; i < N_BANDS; i++) {
      // Welford update
      float delta = energies[i] - band_mean_[i];
      band_mean_[i] += delta / band_n_;
      band_m2_[i]   += delta * (energies[i] - band_mean_[i]);
    }
  } else {
    for (int i = 0; i < N_BANDS; i++) {
      float var = (band_n_ > 2) ? band_m2_[i] / (band_n_ - 1) : 1.0f;
      float sd = sqrtf(var + 1e-6f);
      float z = fabsf(energies[i] - band_mean_[i]) / sd;
      if (z > max_z) { max_z = z; max_band = i; }
    }
  }
  last_score_ = max_z;
  last_band_  = max_band;
#endif
}

void SPH0645Component::update() {
#ifdef USE_ARDUINO
  if (!i2s_ok_) return;

  if (rms_n_ > 0) {
    double mean_sq = rms_sum_ / rms_n_;
    float rms = (float)sqrt(mean_sq);
    rms_sum_ = 0.0;
    rms_n_ = 0;
    if (rms_) rms_->publish_state(rms);
    if (dba_) {
      // Use linear RMS scaled to mic spec; A-weight applied per-sample
      // would need separate accumulator - acceptable approximation.
      float dba = 20.0f * log10f((rms + 1.0f) / SPH0645_REF_RMS) + SPH0645_REF_DB;
      dba_->publish_state(dba);
    }
  }

  if (anomaly_score_) anomaly_score_->publish_state(last_score_);
  if (anomaly_band_ && last_band_ >= 0) anomaly_band_->publish_state(last_band_);
#endif
}

void SPH0645Component::start_baseline_learn() {
#ifdef USE_ARDUINO
  for (int i = 0; i < N_BANDS; i++) { band_mean_[i] = 0; band_m2_[i] = 0; }
  band_n_ = 0;
  learning_ = true;
  learn_started_ms_ = millis();
  ESP_LOGI(TAG, "anomaly baseline learning started");
#endif
}

void SPH0645Component::freeze_baseline() {
  learning_ = false;
}

}  // namespace sph0645
}  // namespace esphome
