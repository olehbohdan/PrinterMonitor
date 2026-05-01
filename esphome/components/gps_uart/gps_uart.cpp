// Minimal NMEA-0183 parser for ATGM332D (or any GPS that emits GGA/RMC).
// Tolerates GP, GN, GL, GB, BD prefixes (multi-constellation receivers).
//
// Why a custom parser instead of TinyGPSPlus?
//   - TinyGPS pulls in extra deps we don't need.
//   - We want full control over publish cadence (every 2 s) and fix-status
//     text (so HA can read "no_fix"/"2d"/"3d"/"dgps").

#include "gps_uart.h"
#include "esphome/core/log.h"
#include <string.h>
#include <stdlib.h>

namespace esphome {
namespace gps_uart {

static const char *const TAG = "gps_uart";

void GPSComponent::setup() {
  ESP_LOGI(TAG, "GPS UART ready");
}

void GPSComponent::dump_config() {
  ESP_LOGCONFIG(TAG, "GPS UART parser");
}

void GPSComponent::loop() {
  while (this->available()) {
    uint8_t c;
    if (!this->read_byte(&c)) break;
    if (c == '\r') continue;
    if (c == '\n') {
      line_[line_len_] = 0;
      if (line_len_ > 6 && line_[0] == '$') parse_(line_);
      line_len_ = 0;
    } else if (line_len_ < kLineMax - 1) {
      line_[line_len_++] = (char)c;
    } else {
      line_len_ = 0;  // drop overlength garbage
    }
  }
  publish_throttled_();
}

static bool starts_with_x(const char *s, char c, const char *tail) {
  // $xxYYY where x is constellation prefix
  if (s[0] != '$') return false;
  if (strlen(s) < 6) return false;
  return s[3] == tail[0] && s[4] == tail[1] && s[5] == tail[2];
}

void GPSComponent::parse_(const char *s) {
  if (starts_with_x(s, 'G', "GGA")) parse_gga_(s);
  else if (starts_with_x(s, 'G', "RMC")) parse_rmc_(s);
}

// Convert NMEA ddmm.mmmm to decimal degrees
static float nmea_to_dec(const char *s, char hemi) {
  if (!s || !*s) return NAN;
  float v = atof(s);
  int deg = (int)(v / 100.0f);
  float min = v - deg * 100.0f;
  float dec = deg + min / 60.0f;
  if (hemi == 'S' || hemi == 'W') dec = -dec;
  return dec;
}

// Tokeniser - splits CSV into fields[], up to N
static int split_csv(char *s, char **fields, int max_n) {
  int n = 0;
  fields[n++] = s;
  while (*s && n < max_n) {
    if (*s == ',') { *s = 0; fields[n++] = s + 1; }
    else if (*s == '*') { *s = 0; break; }
    s++;
  }
  return n;
}

void GPSComponent::parse_gga_(const char *raw) {
  // $GxGGA,UTC,lat,N,lon,E,fix,sats,hdop,alt,M,...,*CK
  char buf[kLineMax]; strncpy(buf, raw, kLineMax - 1); buf[kLineMax - 1] = 0;
  char *f[16];
  int n = split_csv(buf, f, 16);
  if (n < 11) return;
  p_utc_ = f[1] ? std::string(f[1]) : std::string("");
  if (f[2] && *f[2]) p_lat_ = nmea_to_dec(f[2], f[3] ? f[3][0] : 'N');
  if (f[4] && *f[4]) p_lon_ = nmea_to_dec(f[4], f[5] ? f[5][0] : 'E');
  int fix = (f[6] && *f[6]) ? atoi(f[6]) : 0;
  switch (fix) {
    case 0: p_fix_ = "no_fix"; break;
    case 1: p_fix_ = "gps"; break;
    case 2: p_fix_ = "dgps"; break;
    case 4: p_fix_ = "rtk_fixed"; break;
    case 5: p_fix_ = "rtk_float"; break;
    default: p_fix_ = "fix";
  }
  if (f[7] && *f[7]) p_sats_ = atoi(f[7]);
  if (f[8] && *f[8]) p_hdop_ = atof(f[8]);
  if (f[9] && *f[9]) p_alt_ = atof(f[9]);
}

void GPSComponent::parse_rmc_(const char *raw) {
  // $GxRMC,UTC,A/V,lat,N,lon,E,speed_kn,...
  char buf[kLineMax]; strncpy(buf, raw, kLineMax - 1); buf[kLineMax - 1] = 0;
  char *f[14];
  int n = split_csv(buf, f, 14);
  if (n < 8) return;
  if (f[7] && *f[7]) p_speed_ = atof(f[7]) * 1.852f;  // knots -> km/h
}

void GPSComponent::publish_throttled_() {
  uint32_t now = millis();
  if (now - last_publish_ms_ < 2000) return;
  last_publish_ms_ = now;
  if (lat_  && !isnan(p_lat_)) lat_->publish_state(p_lat_);
  if (lon_  && !isnan(p_lon_)) lon_->publish_state(p_lon_);
  if (alt_  && !isnan(p_alt_)) alt_->publish_state(p_alt_);
  if (hdop_ && !isnan(p_hdop_)) hdop_->publish_state(p_hdop_);
  if (sats_ && p_sats_ >= 0) sats_->publish_state((float)p_sats_);
  if (speed_ && !isnan(p_speed_)) speed_->publish_state(p_speed_);
  if (fix_status_ && !p_fix_.empty()) fix_status_->publish_state(p_fix_);
  if (utc_time_ && !p_utc_.empty()) utc_time_->publish_state(p_utc_);
}

}  // namespace gps_uart
}  // namespace esphome
