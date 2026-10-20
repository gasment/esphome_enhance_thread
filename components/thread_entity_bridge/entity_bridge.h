#pragma once
#include "esphome/core/defines.h"
#include "esphome/core/entity_base.h"
#include "esphome/core/component.h"
#include "esphome/core/device.h"
#include "cJSON.h"
#include <functional>
#include <memory>
#include <string>
#include <vector>
#ifdef USE_SENSOR
#include "esphome/components/sensor/sensor.h"
#endif
#ifdef USE_BINARY_SENSOR
#include "esphome/components/binary_sensor/binary_sensor.h"
#endif
#ifdef USE_TEXT_SENSOR
#include "esphome/components/text_sensor/text_sensor.h"
#endif
#ifdef USE_SWITCH
#include "esphome/components/switch/switch.h"
#endif
#ifdef USE_NUMBER
#include "esphome/components/number/number.h"
#endif
#ifdef USE_SELECT
#include "esphome/components/select/select.h"
#endif
#ifdef USE_BUTTON
#include "esphome/components/button/button.h"
#endif
#ifdef USE_TEXT
#include "esphome/components/text/text.h"
#endif
#ifdef USE_LIGHT
#include "esphome/components/light/light_state.h"
#endif
#ifdef USE_FAN
#include "esphome/components/fan/fan.h"
#endif
#ifdef USE_COVER
#include "esphome/components/cover/cover.h"
#endif
#ifdef USE_CLIMATE
#include "esphome/components/climate/climate.h"
#endif
#ifdef USE_LOCK
#include "esphome/components/lock/lock.h"
#endif
#ifdef USE_VALVE
#include "esphome/components/valve/valve.h"
#endif
namespace esphome::thread_entity_bridge {
std::string encode(const cJSON *);
const cJSON *field(const cJSON *, const char *);
std::string string(const cJSON *, const char *, const std::string &fallback = "");
bool validate_capabilities(const std::string &kind, const cJSON *caps);
bool validate_value(const std::string &kind, const cJSON *caps, const cJSON *value, bool command);
struct Endpoint {
  std::string id, kind;
  EntityBase *entity;
  std::function<cJSON *()> describe, read;
  std::function<bool(const cJSON *)> execute;
  std::function<void(std::function<void()>)> subscribe;
};
void register_remote(EntityBase *entity,const std::string &device_class,const std::string &unit,const std::string &icon);
StringRef remote_metadata(EntityBase *entity,uint8_t category,StringRef fallback);
void mark_remote_ready(EntityBase *entity);
bool remote_state_ready(EntityBase *entity);
bool route_light(uint32_t key, uint32_t device, const cJSON *command);
using Sender = std::function<void(const cJSON *)>;
struct Proxy {
  EntityBase *entity{nullptr};
  Component *component{nullptr};
  std::function<void(const cJSON *)> apply;
  virtual ~Proxy() = default;
  virtual void set_device(Device *) = 0;
};
// Caller owns the Proxy for the entire firmware lifetime, including metadata strings.
std::unique_ptr<Proxy> make_proxy(const std::string &kind, const cJSON *caps, Sender sender);
#ifdef USE_SENSOR
Endpoint bind(const std::string &id, sensor::Sensor *source);
#endif
#ifdef USE_BINARY_SENSOR
Endpoint bind(const std::string &id, binary_sensor::BinarySensor *source);
#endif
#ifdef USE_TEXT_SENSOR
Endpoint bind(const std::string &id, text_sensor::TextSensor *source);
#endif
#ifdef USE_SWITCH
Endpoint bind(const std::string &id, switch_::Switch *source);
#endif
#ifdef USE_NUMBER
Endpoint bind(const std::string &id, number::Number *source);
#endif
#ifdef USE_SELECT
Endpoint bind(const std::string &id, select::Select *source);
#endif
#ifdef USE_BUTTON
Endpoint bind(const std::string &id, button::Button *source);
#endif
#ifdef USE_TEXT
Endpoint bind(const std::string &id, text::Text *source);
#endif
#ifdef USE_LIGHT
Endpoint bind(const std::string &id, light::LightState *source);
#endif
#ifdef USE_FAN
Endpoint bind(const std::string &id, fan::Fan *source);
#endif
#ifdef USE_COVER
Endpoint bind(const std::string &id, cover::Cover *source);
#endif
#ifdef USE_CLIMATE
Endpoint bind(const std::string &id, climate::Climate *source);
#endif
#ifdef USE_LOCK
Endpoint bind(const std::string &id, lock::Lock *source);
#endif
#ifdef USE_VALVE
Endpoint bind(const std::string &id, valve::Valve *source);
#endif
}
