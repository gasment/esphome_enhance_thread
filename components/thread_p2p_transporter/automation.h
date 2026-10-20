#pragma once
#include "esphome/core/automation.h"
#include "esphome/core/defines.h"
#include "esphome/core/log.h"
#include "thread_p2p_transporter.h"
#ifdef USE_SENSOR
#include "esphome/components/sensor/sensor.h"
#endif
#ifdef USE_BINARY_SENSOR
#include "esphome/components/binary_sensor/binary_sensor.h"
#endif
#ifdef USE_TEXT_SENSOR
#include "esphome/components/text_sensor/text_sensor.h"
#endif

namespace esphome::thread_p2p_transporter {

class ReceiveTrigger : public Trigger<uint8_t, Payload> {
 public:
  explicit ReceiveTrigger(ThreadP2PTransporter *parent) {
    parent->add_on_receive_callback([this](uint8_t flag, Payload payload) { this->trigger(flag, payload); });
  }
};
class SendSuccessTrigger : public Trigger<uint8_t, uint32_t> {
 public:
  explicit SendSuccessTrigger(ThreadP2PTransporter *parent) {
    parent->add_on_send_success_callback([this](uint8_t flag, uint32_t id) { this->trigger(flag, id); });
  }
};
class SendFailTrigger : public Trigger<uint8_t, uint32_t> {
 public:
  explicit SendFailTrigger(ThreadP2PTransporter *parent) {
    parent->add_on_send_fail_callback([this](uint8_t flag, uint32_t id) { this->trigger(flag, id); });
  }
};

template<typename... Ts> class SendAction : public Action<Ts...> {
 public:
  explicit SendAction(ThreadP2PTransporter *parent) : parent_(parent) {}
  TEMPLATABLE_VALUE(std::string, target)
  TEMPLATABLE_VALUE(uint16_t, port)
  TEMPLATABLE_VALUE(uint8_t, send_flag)
  TEMPLATABLE_VALUE(bool, bool_payload)
  TEMPLATABLE_VALUE(float, float_payload)
  TEMPLATABLE_VALUE(std::string, string_payload)
  void set_has_port(bool value) { has_port_ = value; }
  void set_target_is_address(bool value) { direct_ = value; }
  void set_payload_kind(uint8_t value) { kind_ = PayloadType(value); }
  void set_ack(uint32_t timeout, uint8_t resend) { timeout_ = timeout; resend_ = resend; }
  void play(const Ts &...x) override {
    Payload payload;
    if (kind_ == PayloadType::BOOL) payload = Payload::from_bool(bool_payload_.value(x...));
    else if (kind_ == PayloadType::FLOAT) payload = Payload::from_float(float_payload_.value(x...));
    else payload = Payload::from_string(string_payload_.value(x...));
    const auto port = has_port_ ? std::optional<uint16_t>(port_.value(x...)) : std::nullopt;
    parent_->send(target_.value(x...), direct_, port, send_flag_.value(x...), payload, timeout_, resend_);
  }
 protected:
  ThreadP2PTransporter *parent_;
  bool direct_{false}, has_port_{false};
  PayloadType kind_{PayloadType::STRING};
  uint32_t timeout_{0};
  uint8_t resend_{0};
};

// Values come from the automation's own argument tuple, including after delays.
// Never read mutable global "last received" state.
inline const Payload *context_payload(uint8_t, const Payload &payload) { return &payload; }
template<typename... Ts> const Payload *context_payload(const Ts &...) { return nullptr; }
inline int context_flag(uint8_t flag, const Payload &) { return flag; }
template<typename... Ts> int context_flag(const Ts &...) { return -1; }

template<typename... Ts> class FlagCondition : public Condition<Ts...> {
 public:
  explicit FlagCondition(uint8_t expected) : expected_(expected) {}
  bool check(const Ts &...x) override { return context_flag(x...) == expected_; }
 protected:
  uint8_t expected_;
};

template<typename... Ts> class PayloadCondition : public Condition<Ts...> {
 public:
  void set_bool(bool value) { expected_ = Payload::from_bool(value); }
  void set_float(float value) { expected_ = Payload::from_float(value); }
  void set_string(const std::string &value) { expected_ = Payload::from_string(value); }
  void set_above(float value) { range_ = true; above_ = value; }
  void set_below(float value) { range_ = true; below_ = value; }
  bool check(const Ts &...x) override {
    const auto *p = context_payload(x...);
    if (!p) return false;
    if (range_) return p->is_float() && (std::isnan(above_) || p->float_value > above_) &&
                                                   (std::isnan(below_) || p->float_value < below_);
    if (p->type != expected_.type) return false;
    if (p->is_bool()) return p->bool_value == expected_.bool_value;
    if (p->is_float()) return p->float_value == expected_.float_value;
    return p->string_value == expected_.string_value;
  }
 protected:
  Payload expected_;
  bool range_{false};
  float above_{NAN}, below_{NAN};
};

template<typename... Ts> class PublishAction : public Action<Ts...> {
 public:
#ifdef USE_SENSOR
  void set_sensor_id(sensor::Sensor *value) { sensor_ = value; }
#endif
#ifdef USE_BINARY_SENSOR
  void set_binary_sensor_id(binary_sensor::BinarySensor *value) { binary_ = value; }
#endif
#ifdef USE_TEXT_SENSOR
  void set_text_sensor_id(text_sensor::TextSensor *value) { text_ = value; }
#endif
  void play(const Ts &...x) override {
    const auto *p = context_payload(x...);
    if (p) {
#ifdef USE_SENSOR
      if (sensor_ && p->is_float()) { sensor_->publish_state(p->float_value); return; }
#endif
#ifdef USE_BINARY_SENSOR
      if (binary_ && p->is_bool()) { binary_->publish_state(p->bool_value); return; }
#endif
#ifdef USE_TEXT_SENSOR
      if (text_ && p->is_string()) { text_->publish_state(p->string_value); return; }
#endif
    }
    ESP_LOGW("thread_p2p_transporter", "Publish skipped: missing receive context or mismatched payload type");
  }
 protected:
#ifdef USE_SENSOR
  sensor::Sensor *sensor_{nullptr};
#endif
#ifdef USE_BINARY_SENSOR
  binary_sensor::BinarySensor *binary_{nullptr};
#endif
#ifdef USE_TEXT_SENSOR
  text_sensor::TextSensor *text_{nullptr};
#endif
};

}  // namespace esphome::thread_p2p_transporter
