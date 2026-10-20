#pragma once

#include <atomic>
#include <cmath>
#include <functional>
#include <optional>
#include <string>
#include <vector>
#include "esphome/core/component.h"
#include "esphome/core/helpers.h"
#include "esphome/components/openthread/openthread.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "openthread/ip6.h"
#include "openthread/dataset.h"
#include "openthread/udp.h"

namespace esphome::thread_p2p_transporter {

enum class PayloadType : uint8_t { BOOL = 1, FLOAT = 2, STRING = 3 };
struct Payload {
  PayloadType type{PayloadType::STRING};
  bool bool_value{false};
  float float_value{0};
  std::string string_value;
  bool is_bool() const { return type == PayloadType::BOOL; }
  bool is_float() const { return type == PayloadType::FLOAT; }
  bool is_string() const { return type == PayloadType::STRING; }
  bool as_bool() const { return bool_value; }
  float as_float() const { return float_value; }
  const std::string &as_string() const { return string_value; }
  static Payload from_bool(bool value) { Payload p; p.type = PayloadType::BOOL; p.bool_value = value; return p; }
  static Payload from_float(float value) { Payload p; p.type = PayloadType::FLOAT; p.float_value = value; return p; }
  static Payload from_string(const std::string &value) { Payload p; p.string_value = value; return p; }
};

class ThreadP2PTransporter : public Component {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  void on_shutdown() override;
  float get_setup_priority() const override { return setup_priority::LATE; }
  void set_sender_only(bool value) { sender_only_ = value; }
  void set_address(const std::string &value, uint16_t port) { address_text_ = value; port_ = port; }
  void add_target(const std::string &id, const std::string &address, uint16_t port) {
    targets_.push_back({id, address, {}, port});
  }
  bool is_ready() const { return !shutdown_ && ready_ && ot_ != nullptr && ot_->is_connected(); }
  bool send(const std::string &target, bool direct_address, std::optional<uint16_t> port_override, uint8_t flag,
            const Payload &payload, uint32_t ack_timeout = 0, uint8_t resend = 0);
  void add_on_receive_callback(std::function<void(uint8_t, Payload)> fn) { receive_callbacks_.add(std::move(fn)); }
  void add_on_send_success_callback(std::function<void(uint8_t, uint32_t)> fn) { success_callbacks_.add(std::move(fn)); }
  void add_on_send_fail_callback(std::function<void(uint8_t, uint32_t)> fn) { fail_callbacks_.add(std::move(fn)); }

 protected:
  static constexpr size_t MAX_DATAGRAM = 1232, HEADER_SIZE = 24, MAX_PAYLOAD = MAX_DATAGRAM - HEADER_SIZE;
  static constexpr size_t MAX_PENDING = 8, MAX_SEEN = 128;
  static constexpr uint32_t DEDUP_TTL = 900000;
  struct Target { std::string id, address_text; otIp6Address address; uint16_t port; };
  struct Packet { otIp6Address source; uint16_t port; uint16_t size; uint8_t data[MAX_DATAGRAM]; };
  struct Pending {
    otIp6Address destination;
    uint16_t port;
    uint8_t flag, retries;
    uint32_t id, timeout, started;
    std::string wire;
  };
  struct Seen {
    otIp6Address source;
    uint16_t port;
    uint64_t session;
    uint32_t id, at;
  };
  static void receive_(void *context, otMessage *message, const otMessageInfo *info);
  bool open_();
  bool transmit_(const otIp6Address &target, uint16_t port, const std::string &wire);
  void process_(const Packet &packet);
  void notify_fail_(uint8_t flag, uint32_t id);
  openthread::OpenThreadComponent *ot_{nullptr};
  otUdpSocket socket_{};
  otIp6Address address_{};
  otMeshLocalPrefix prefix_{};
  QueueHandle_t queue_{nullptr};
  std::vector<Target> targets_;
  std::vector<Pending> pending_;
  std::vector<Seen> seen_;
  std::string address_text_;
  uint64_t session_{0};
  uint32_t sequence_{0};
  uint16_t port_{18520};
  bool sender_only_{false};
  bool address_added_{false}, socket_open_{false}, ready_{false};
  bool retry_pending_{false};
  uint32_t retry_at_{0}, received_{0}, sent_{0}, rejected_{0};
  std::atomic<bool> shutdown_{false};
  std::atomic<uint32_t> dropped_{0};
  CallbackManager<void(uint8_t, Payload)> receive_callbacks_;
  CallbackManager<void(uint8_t, uint32_t)> success_callbacks_, fail_callbacks_;
};

}  // namespace esphome::thread_p2p_transporter
