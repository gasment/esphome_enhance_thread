#pragma once

#include <atomic>
#include <string>
#include "esphome/core/component.h"
#include "esphome/core/optional.h"
#include "esphome/components/binary_sensor/binary_sensor.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/text_sensor/text_sensor.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_openthread_types.h"

namespace esphome::thread_gateway { class ThreadGateway; }
namespace esphome::thread_br {

class ThreadBorderRouter : public PollingComponent {
 public:
  bool is_thread_ready() const { return state_ == State::READY; }
  void set_thread_gateway(thread_gateway::ThreadGateway *thread_gateway) { thread_gateway_ = thread_gateway; }
  void setup() override;
  void loop() override;
  void update() override;
  void dump_config() override;
  void on_shutdown() override;
  float get_setup_priority() const override { return setup_priority::AFTER_WIFI; }
  void set_uart_pins(int tx, int rx) { tx_ = tx; rx_ = rx; }
  void set_rcp_pins(int reset, int boot) { reset_ = reset; boot_ = boot; }
  void set_coexistence_pins(int request, int priority, int grant, int tx_line) {
    request_ = request; priority_ = priority; grant_ = grant; tx_line_ = tx_line;
  }
  void set_force_dataset(bool force) { force_dataset_ = force; }
  void set_rcp_activity_led_enabled(bool enabled) {
    queue_rcp_led_action_(1, enabled);
  }
  void set_rcp_connection_led_enabled(bool enabled) {
    queue_rcp_led_action_(2, enabled);
  }
  optional<bool> get_rcp_activity_led_enabled() const { return rcp_led_state_(1); }
  optional<bool> get_rcp_connection_led_enabled() const { return rcp_led_state_(2); }
  void set_web(uint16_t port, const std::string &auth, bool psram) { web_port_ = port; auth_ = auth; use_psram_ = psram; }
  void set_rcp_connectivity(binary_sensor::BinarySensor *s) { rcp_connectivity_ = s; }
  void set_thread_attached(binary_sensor::BinarySensor *s) { thread_attached_ = s; }
  void set_thread_role(text_sensor::TextSensor *s) { thread_role_ = s; }
  void set_rcp_version(text_sensor::TextSensor *s) { rcp_version_ = s; }
  void set_neighbor_count(sensor::Sensor *s) { neighbor_count_ = s; }
  void set_child_count(sensor::Sensor *s) { child_count_ = s; }

 protected:
  enum class State { WAITING, STARTING, READY, FAILED, STOPPING };
  static ThreadBorderRouter *instance_;
  thread_gateway::ThreadGateway *thread_gateway_{nullptr};
  static void start_task_(void *arg);
  static void web_task_(void *arg);
  static void rcp_failure_();
  static void rcp_reset_failure_();
  void start_();
  void reset_rcp_();
  bool initialize_();
  void sample_();
  void service_backbone_();
  bool send_rcp_led_control_(uint8_t operation, uint8_t update_mask, uint8_t values,
                             bool healthy, uint16_t sequence);  // Caller holds the OpenThread lock.
  void queue_rcp_led_action_(uint8_t bit, bool enabled);
  optional<bool> rcp_led_state_(uint8_t bit) const;
  void service_rcp_leds_();
  std::atomic<State> state_{State::WAITING};
  std::atomic<bool> rcp_fault_{false};
  std::atomic<uint32_t> reset_count_{0};
  std::atomic<bool> healthy_{false};
  std::atomic<unsigned> role_{0}, neighbors_{0}, children_{0};
  std::atomic<bool> want_web_{false};
  std::atomic<bool> want_backbone_{false};
  // Only the startup worker owns the BR lifecycle below.
  bool border_router_started_{false}, backbone_up_{false}, backbone_retry_{false};
  uint32_t backbone_attempt_at_{0};
  std::atomic<uint8_t> rcp_led_pending_{0};  // High bits: update mask; low bits: values.
  uint8_t led_inflight_{0}, led_attempts_{0};  // Startup worker owns retry state.
  uint16_t led_sequence_{0};
  uint32_t led_last_attempt_{0}, led_last_query_{0};
  bool force_dataset_{false}, use_psram_{false};
  bool failure_reported_{false};
  static constexpr int UART_PORT = 1;
  static constexpr int BAUD_RATE = 460800;
  int tx_{18}, rx_{17}, reset_{7}, boot_{8};
  int request_{12}, priority_{10}, grant_{13}, tx_line_{11};
  uint16_t web_port_{80};
  TaskHandle_t startup_task_{nullptr};
  std::string auth_;
  std::string version_;
  // IDF's worker retains this pointer for its whole lifetime.
  esp_openthread_config_t ot_config_{};
  binary_sensor::BinarySensor *rcp_connectivity_{nullptr}, *thread_attached_{nullptr};
  text_sensor::TextSensor *thread_role_{nullptr}, *rcp_version_{nullptr};
  sensor::Sensor *neighbor_count_{nullptr}, *child_count_{nullptr};
};

}  // namespace esphome::thread_br
