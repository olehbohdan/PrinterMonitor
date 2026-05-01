#pragma once

#include "esphome/core/component.h"
#include "esphome/components/uart/uart.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/text_sensor/text_sensor.h"

namespace esphome {
namespace gps_uart {

class GPSComponent : public Component, public uart::UARTDevice {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::LATE; }

  void set_lat(sensor::Sensor *s) { lat_ = s; }
  void set_lon(sensor::Sensor *s) { lon_ = s; }
  void set_alt(sensor::Sensor *s) { alt_ = s; }
  void set_hdop(sensor::Sensor *s) { hdop_ = s; }
  void set_sats(sensor::Sensor *s) { sats_ = s; }
  void set_speed(sensor::Sensor *s) { speed_ = s; }
  void set_fix_status(text_sensor::TextSensor *s) { fix_status_ = s; }
  void set_utc_time(text_sensor::TextSensor *s) { utc_time_ = s; }

 protected:
  sensor::Sensor *lat_{nullptr};
  sensor::Sensor *lon_{nullptr};
  sensor::Sensor *alt_{nullptr};
  sensor::Sensor *hdop_{nullptr};
  sensor::Sensor *sats_{nullptr};
  sensor::Sensor *speed_{nullptr};
  text_sensor::TextSensor *fix_status_{nullptr};
  text_sensor::TextSensor *utc_time_{nullptr};

  static constexpr int kLineMax = 96;
  char line_[kLineMax]{};
  int  line_len_{0};

  uint32_t last_publish_ms_{0};

  void parse_(const char *s);
  void parse_gga_(const char *s);
  void parse_rmc_(const char *s);
  void publish_throttled_();

  // Pending values updated as sentences arrive; published every 2 s.
  float p_lat_{NAN}, p_lon_{NAN}, p_alt_{NAN}, p_hdop_{NAN}, p_speed_{NAN};
  int   p_sats_{-1};
  std::string p_fix_;
  std::string p_utc_;
};

}  // namespace gps_uart
}  // namespace esphome
