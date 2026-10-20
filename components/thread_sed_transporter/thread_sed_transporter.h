#pragma once
#include "esphome/core/defines.h"

#include <vector>
#include <atomic>
#include <functional>
#include "esphome/components/openthread/openthread.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "openthread/udp.h"
#include "cJSON.h"
#include "esphome/components/thread_entity_bridge/entity_bridge.h"
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
#include <cmath>

#include "esphome/core/component.h"
#include "esphome/core/helpers.h"
#include "esphome/core/preferences.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/binary_sensor/binary_sensor.h"
#include "esphome/components/text_sensor/text_sensor.h"
#include "delta.h"
#include "text_record.h"

namespace esphome::thread_sed_transporter {

struct RetainedSample {
  uint32_t version{0};
  float baseline{0};
};

class ReportEntry {
 public:
  virtual ~ReportEntry() = default;
  virtual bool initialize(bool restore) = 0;
  virtual ReportOutcome stage() = 0;
  virtual bool commit() = 0;
  virtual cJSON *value() const = 0;
  void set_identity(const std::string &id) { id_ = id; }
  const std::string &identity() const { return id_; }
  virtual cJSON *descriptor() const { return endpoint_.describe(); }
  ReportOutcome outcome{ReportOutcome::INVALID};
  void dump_config();
  void set_storage_key(uint32_t key) { this->storage_key_ = key; }
  void prepare_acquisition() { this->source_ready_ = !this->requires_fresh_source_; }
  bool source_ready() const { return this->source_ready_; }

 protected:
  std::string id_;
  thread_entity_bridge::Endpoint endpoint_{};
  ESPPreferenceObject preference_;
  uint32_t storage_key_{0};
  bool requires_fresh_source_{false};
  bool source_ready_{true};
};

class ReportSensor : public ReportEntry {
 public:
  void set_delta(float delta) { this->delta_ = delta; }
  void set_source(sensor::Sensor *source, bool require_fresh) {
    this->endpoint_ = thread_entity_bridge::bind(id_, source);
    this->requires_fresh_source_ = require_fresh;
    source->add_on_state_callback([this](float value) { this->source_ready_ = std::isfinite(value); });
  }
  bool initialize(bool restore) override;
  ReportOutcome stage() override;
  bool commit() override;
  cJSON *value() const override;

 protected:
  optional<float> pending_;
  RetainedSample sample_{};
  float delta_{0};
};

class ReportBinarySensor : public ReportEntry {
 public:
  void set_initial_state_metadata(bool value) { trigger_on_initial_state_ = value; }
  cJSON *descriptor() const override {
    auto *json = ReportEntry::descriptor();
    if (json) cJSON_AddBoolToObject(json, "trigger_on_initial_state", trigger_on_initial_state_);
    return json;
  }
  void set_source(binary_sensor::BinarySensor *source, bool require_fresh) {
    this->endpoint_ = thread_entity_bridge::bind(id_, source);
    this->requires_fresh_source_ = require_fresh;
    source->add_on_state_callback([this](bool) { this->source_ready_ = true; });
  }
  bool initialize(bool restore) override;
  ReportOutcome stage() override;
  bool commit() override;
  cJSON *value() const override;

 protected:
  bool trigger_on_initial_state_{true};
  optional<bool> pending_;
  struct Sample {
    uint32_t version{0};
    uint32_t baseline{0};
  } sample_;
};

class ReportTextSensor : public ReportEntry {
 public:
  void set_source(text_sensor::TextSensor *source, bool require_fresh) {
    this->endpoint_ = thread_entity_bridge::bind(id_, source);
    this->requires_fresh_source_ = require_fresh;
    source->add_on_state_callback([this](const std::string &) { this->source_ready_ = true; });
  }
  void set_max_length(uint32_t length) { this->max_length_ = length; }
  cJSON *descriptor() const override {
    auto *json = ReportEntry::descriptor();
    if (json) cJSON_SetNumberValue(cJSON_GetObjectItemCaseSensitive(json, "max_length"), max_length_);
    return json;
  }
  bool initialize(bool restore) override;
  ReportOutcome stage() override;
  bool commit() override;
  cJSON *value() const override;

 protected:
  optional<std::string> pending_;
  TextRecord record_{};
  uint32_t max_length_{32};
  std::string baseline_;
  bool has_baseline_{false};
};


class ThreadSedTransporter : public Component {
 public:
  // Application-layer gateway reachability; Thread diagnostics belong to openthread_info.
  bool is_gateway_connencted() const {
    return !shutdown_ && !is_failed() && opened_ && ot_ != nullptr && ot_->is_connected() && confirmed_ &&
           target_valid_() && uint32_t(millis() - last_gateway_reply_) < heartbeat_ms_ * 3;
  }
  void discovery();
  void setup() override;
  void loop() override;
  void report(uint32_t ack_timeout = 0, uint8_t ack_resend = 0);
  void dump_config() override;
  void on_shutdown() override;
  float get_setup_priority() const override { return setup_priority::LATE; }
  void set_openthread(openthread::OpenThreadComponent *ot) { ot_ = ot; }
  void set_node(const std::string &id, const std::string &name, uint32_t revision) {
    node_ = id; name_ = name; revision_ = revision;
  }
  void set_discovery(const std::string &id, const std::string &group, uint16_t port, uint32_t timeout) {
    expected_gateway_ = id; group_ = group; discovery_port_ = port; discovery_timeout_ = timeout;
  }
  void set_timing(uint32_t delay, uint32_t timeout, uint16_t every, uint32_t heartbeat) {
    acquisition_delay_ = delay; timeout_ms_ = timeout; full_every_ = every; heartbeat_ms_ = heartbeat;
  }
  void set_storage_identity(uint32_t identity) { storage_identity_ = identity; }
  void set_control_ready(bool value) { ready_reader_ = [value]() { return value; }; }
  void set_control_ready_reader(std::function<bool()> reader) { ready_reader_ = std::move(reader); }
  void add_sensor(ReportEntry *sensor) { sensors_.push_back(sensor); }
  void add_update_component(PollingComponent *component);
  template<class T> void add_control(const std::string &id, T *source) {
    auto endpoint = thread_entity_bridge::bind(id, source);
    Control control;
    control.id = id;
    control.type = endpoint.kind;
    control.descriptor = endpoint.describe;
    control.read = endpoint.read;
    control.execute = [endpoint](const cJSON *value) {
      auto *description = endpoint.describe();
      const bool valid = description && thread_entity_bridge::validate_value(
          endpoint.kind, thread_entity_bridge::field(description, "capabilities"), value, true);
      cJSON_Delete(description);
      return valid && endpoint.execute(value);
    };
    controls_.push_back(std::move(control));
  }
  template<typename F> void add_on_complete_callback(F &&callback) { complete_callbacks_.add(std::forward<F>(callback)); }
  template<typename F> void add_on_report_success_callback(F &&callback) { success_callbacks_.add(std::forward<F>(callback)); }
  template<typename F> void add_on_report_fail_callback(F &&callback) { fail_callbacks_.add(std::forward<F>(callback)); }

 protected:
  struct Control {
    std::string id, type, previous;
    std::function<cJSON *()> descriptor, read;
    std::function<bool(const cJSON *)> execute;
  };
  struct Packet { otIp6Address peer; uint16_t port; size_t size; char data[1233]; };
  struct CompletedCommand { uint32_t id; std::string response; };
  struct PendingAck {
    uint32_t id, started, attempt_at, timeout;
    uint8_t resend_limit{0}, resends{0};
    bool recovered{false}, recovering{false};
    uint32_t recovery_at{0};
    std::vector<std::string> payloads;
    uint16_t parts, port;
    otIp6Address peer;
    std::string gateway;
    enum class Result { WAITING, SUCCESS } result{Result::WAITING};
  };
  enum class Phase { IDLE, DELAY, ACQUIRE, SEND };
  static void receive_(void *, otMessage *, const otMessageInfo *);
  bool open_();
  bool send_(const otIp6Address &, uint16_t, const std::string &);
  bool target_valid_() const;
  void renew_target_();
  bool ensure_target_();
  void process_(const Packet &);
  cJSON *envelope_(const char *type) const;
  bool prepare_manifest_();
  void service_manifest_();
  void stage_();
  void transmit_();
  void service_report_ack_();
  void receive_report_ack_(const Packet &packet, const cJSON *json);
  void track_report_ack_(std::vector<std::string> payloads);
  void report_result_(bool success, uint32_t report_id);
  void finish_();
  void presence_(bool ready, bool sleeping = false);
  void command_(const cJSON *);
  void service_presence_();
  bool ready_() const { return ready_reader_ && ready_reader_(); }
  std::function<bool()> ready_reader_;
  std::vector<ReportEntry *> sensors_;
  std::vector<PollingComponent *> update_components_;
  std::vector<Control> controls_;
  std::vector<std::string> manifest_;
  std::vector<CompletedCommand> commands_;
  std::vector<PendingAck> pending_acks_;
  uint32_t next_report_id_{0}, report_id_{0}, report_ack_timeout_{0};
  uint8_t report_ack_resend_{0};
  bool need_ack_{false}, ack_tracked_{false}, ack_immediate_failure_{false};
  openthread::OpenThreadComponent *ot_{nullptr};
  otUdpSocket socket_{};
  QueueHandle_t packets_{nullptr};
  std::atomic<bool> shutdown_{false};
  std::atomic<uint32_t> drops_{0};
  std::string node_, name_, session_, expected_gateway_, group_{"ff13::8000:8562"};
  uint32_t revision_{1}, storage_identity_{0}, sequence_{0}, request_id_{0};
  uint16_t discovery_port_{18512}, full_every_{20};
  uint32_t discovery_timeout_{1000}, timeout_ms_{5000}, acquisition_delay_{0}, heartbeat_ms_{10000};
  uint32_t started_{0}, query_at_{0}, manifest_at_{0}, heartbeat_at_{0};
  uint32_t last_gateway_reply_{0}, discovery_started_{0};
  bool discovery_active_{false};
  uint8_t query_attempts_{0}, manifest_attempts_{0};
  bool opened_{false}, querying_{false}, confirmed_{false}, reporting_{false}, staged_{false};
  bool last_ready_{false}, presence_dirty_{true}, force_full_{false};
  Phase phase_{Phase::IDLE};
  ReportSummary summary_;
  CallbackManager<void(bool, uint32_t, uint32_t, uint32_t, uint32_t)> complete_callbacks_;
  CallbackManager<void(uint32_t)> success_callbacks_, fail_callbacks_;
};
}  // namespace esphome::thread_sed_transporter
