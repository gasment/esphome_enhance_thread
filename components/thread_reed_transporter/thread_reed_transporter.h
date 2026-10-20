#pragma once
#include "esphome/core/component.h"
#include "esphome/core/hal.h"
#include "esphome/components/openthread/openthread.h"
#include "esphome/components/thread_entity_bridge/entity_bridge.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "openthread/udp.h"
#include <atomic>
namespace esphome::thread_reed_transporter {
class ThreadReedTransporter : public Component {
 public:
  bool is_gateway_connencted() const {
    return !shutdown_ && !is_failed() && ot_ != nullptr && ot_->is_connected() &&
           confirmed_ && target_ && uint32_t(millis()-last_reply_) < heartbeat_ms_*3;
  }
  void discovery();
  void setup() override;
  void loop() override;
  void on_shutdown() override;
  void dump_config() override;
  float get_setup_priority() const override {return setup_priority::LATE;}
  void set_openthread(openthread::OpenThreadComponent *ot) {ot_=ot;}
  void set_node(const std::string &id,const std::string &name,uint32_t rev) {node_=id;name_=name;revision_=rev;}
  void set_discovery(const std::string &group,uint16_t port) {group_=group;discovery_port_=port;}
  void set_gateway_id(const std::string &id) {expected_gateway_=id;}
  void set_timing(uint32_t heartbeat,uint32_t batch,uint32_t sync,uint32_t timeout,uint8_t resend) {
    heartbeat_ms_=heartbeat;batch_ms_=batch;sync_ms_=sync;ack_ms_=timeout;resend_=resend;
  }
  template<class T> void add_entity(const std::string &id,T *source) {
    entries_.push_back({thread_entity_bridge::bind(id,source)});
  }
 protected:
  struct Entry {thread_entity_bridge::Endpoint endpoint; uint32_t generation{1}; bool dirty{true};};
  struct Packet {otIp6Address peer;uint16_t port;size_t size;char data[1233];};
  struct Sample {size_t index;uint32_t generation;};
  struct Pending {
    uint32_t id{0}, at{0};
    uint8_t retries{0};
    size_t next_page{0};
    uint32_t send_at{0};
    std::vector<std::string> pages;
    std::vector<Sample> samples;
    bool recovered{false}, recovering{false};
    uint32_t recovery_at{0};
  };
  static void receive_(void *,otMessage *,const otMessageInfo *);
  bool open_();
  bool send_(const otIp6Address &,uint16_t,const std::string &);
  cJSON *envelope_(const char *) const;
  void discover_();
  void process_(const Packet &);
  void presence_();
  bool manifest_();
  void report_();
  void command_(const cJSON *);
  void invalidate_(bool preserve_report = false);
  void request_full_sync_();
  openthread::OpenThreadComponent *ot_{nullptr};
  otUdpSocket socket_{};QueueHandle_t queue_{nullptr};
  std::atomic<bool> shutdown_{false};std::atomic<uint32_t> dropped_{0};
  bool opened_{false},target_{false},confirmed_{false},querying_{false},check_pending_{false};
  otIp6Address peer_{};uint16_t port_{0};
  std::string node_,name_,session_,gateway_,expected_gateway_,group_{"ff13::8000:8562"};
  uint16_t discovery_port_{18512};
  uint32_t revision_{1},sequence_{0},request_{0},ttl_ms_{0},target_at_{0};
  uint32_t heartbeat_ms_{10000},batch_ms_{100},sync_ms_{60000},ack_ms_{2000};uint8_t resend_{2};
  uint32_t query_at_{0},heartbeat_at_{0},manifest_at_{0},batch_at_{0},sync_at_{0},report_id_{0},last_reply_{0};
  std::vector<Entry> entries_;std::vector<std::string> manifest_pages_;
  size_t manifest_next_{0};uint32_t manifest_send_at_{0};
  Pending pending_;
  struct Result {uint32_t id;std::string payload;};std::vector<Result> results_;
};
}
