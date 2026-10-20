#pragma once
#include "esphome/core/defines.h"
#ifdef USE_THREAD_GATEWAY
#include <array>
#include <atomic>
#include <memory>
#include <functional>
#include <string>
#include <vector>
#include "esphome/core/component.h"
#include "esphome/core/device.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/binary_sensor/binary_sensor.h"
#include "esphome/components/text_sensor/text_sensor.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "openthread/udp.h"
#include "cJSON.h"
#include "esphome/components/thread_entity_bridge/entity_bridge.h"

namespace esphome::thread_br { class ThreadBorderRouter; }
namespace esphome::thread_gateway {

// Registry objects are configured only at boot, before Native API starts.
class RemoteSensor : public sensor::Sensor {
 public:
  void set_node_device(Device *device) { this->set_device_(device); }
};
class RemoteBinary : public binary_sensor::BinarySensor {
 public:
  void set_node_device(Device *device) { this->set_device_(device); }
};
class RemoteText : public text_sensor::TextSensor {
 public:
  void set_node_device(Device *device) { this->set_device_(device); }
};

class ThreadGateway : public Component {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  void on_shutdown() override { shutting_down_ = true; }
  float get_setup_priority() const override { return setup_priority::HARDWARE; }
  void set_reed_heartbeat_misses(uint8_t count) {reed_heartbeat_misses_=count;}
  void set_parent(thread_br::ThreadBorderRouter *parent) { parent_ = parent; }
  void set_gateway_id(const std::string &value) { gateway_id_ = value; }
  void set_auto_register(bool value) { auto_register_ = value; }
  void set_discovery(const std::string &group, uint16_t port, uint32_t ttl, uint32_t interval) {
    group_ = group; discovery_port_ = port; address_ttl_ = ttl; announce_ms_ = interval * 1000;
  }
  void set_data_port(uint16_t value) { data_port_ = value; }
  void set_capacity(uint16_t numeric, uint16_t binary, uint16_t text) { capacity_[0]=numeric; capacity_[1]=binary; capacity_[2]=text; }
  void set_control_capacity(uint8_t kind, uint16_t count) { if(kind<capacity_.size()) capacity_[kind]=count; }
  void send_control(const std::string &node,const std::string &entity,const cJSON *value);
  void set_registry_limit(size_t bytes) { registry_limit_ = bytes; }
  void add_sensor_report_callback(const std::string &node, const std::string &entity, std::function<void(float)> callback) {
    sensor_report_routes_.push_back({node, entity, std::move(callback)});
  }
  void add_binary_report_callback(const std::string &node, const std::string &entity, std::function<void(bool)> callback) {
    binary_report_routes_.push_back({node, entity, std::move(callback)});
  }
  void add_text_report_callback(const std::string &node, const std::string &entity, std::function<void(std::string)> callback) {
    text_report_routes_.push_back({node, entity, std::move(callback)});
  }
  void add_metadata(uint8_t category, const std::string &value, uint8_t index);
  cJSON *web_get(const char *path, int &status);
  cJSON *web_post(const char *path, const cJSON *request, int &status);

 protected:
  enum class Kind : uint8_t { NUMERIC, BINARY, TEXT, SWITCH, NUMBER, SELECT, BUTTON, INPUT_TEXT, LIGHT, FAN, COVER, CLIMATE, LOCK, VALVE };
  struct Entity {
    std::string id, name, unit, device_class, icon, state_class, entity_category, value;
    std::string capabilities;
    Kind kind{Kind::NUMERIC};
    bool approved{false}, has_value{false};
    bool disabled_by_default{false}, force_update{false}, trigger_on_initial_state{false};
    int8_t decimals{0};
    double minimum{0}, maximum{100}, step{1};
    uint16_t max_length{128};
    std::vector<std::string> options;
    uint32_t value_at{0}, source_sequence{0};
    bool has_source_sequence{false};
  };
  struct Manifest {
    std::string name;
    uint32_t revision{0}, started_at{0};
    unsigned parts{0};
    std::vector<bool> received;
    std::vector<std::vector<Entity>> pages;
  };
  struct ReportReceipt {
    uint32_t id{0}, started{0}, base_seq{0};
    uint16_t parts{0}, received{0};
  };
  struct Node {
    std::string id, name, session, address, mode{"unknown"}, debug, last_packet, last_error;
    std::string node_type{"sed"};
    uint32_t heartbeat_ms{10000};
    uint16_t port{0};
    uint32_t last_seen{0}, control_seen{0}, revision{0}, sequence{0};
    bool seen{false}, has_sequence{false}, manifest_requested{false};
    uint32_t manifest_request_at{0};
    uint32_t received{0}, duplicates{0}, errors{0};
    std::vector<std::string> retired_sessions;
    std::vector<Entity> entities;
    Manifest manifest;
    std::vector<ReportReceipt> report_receipts;
  };
  struct LiveDevice {
    std::string node, name;
    Device device;
  };
  struct LiveEntity {
    std::string node, id, name, display_name;
    Kind kind;
    RemoteSensor *numeric{nullptr};
    RemoteBinary *binary{nullptr};
    RemoteText *text{nullptr};
    std::unique_ptr<thread_entity_bridge::Proxy> proxy;
    bool notify_on_sync{true};
  };
  struct Command {
    std::string node, session, entity, payload, status{"queued"}, error;
    uint32_t id{0}, last_attempt{0};
    uint8_t attempts{0};
  };
  struct Packet {
    otIp6Address address;
    uint16_t port;
    bool discovery;
    size_t size;
    char data[1233];
  };
  template<typename T> struct ReportRoute {
    std::string node, entity;
    std::function<void(T)> callback;
  };
  struct ReportEvent {
    std::string node, entity, text;
    Kind kind;
    float number{0};
    bool binary{false};
  };
  struct Lock {
    SemaphoreHandle_t mutex;
    bool acquired;
    explicit Lock(SemaphoreHandle_t m) : mutex(m), acquired(m && xSemaphoreTakeRecursive(m, pdMS_TO_TICKS(100)) == pdTRUE) {}
    void release() { if (acquired) { xSemaphoreGiveRecursive(mutex); acquired = false; } }
    ~Lock() { release(); }
  };
  static void receive_discovery_(void *, otMessage *, const otMessageInfo *);
  static void receive_data_(void *, otMessage *, const otMessageInfo *);
  void receive_(bool discovery, otMessage *message, const otMessageInfo *info);
  bool start_transport_();
  bool send_(const otIp6Address &address, uint16_t port, const std::string &payload, bool discovery = false);
  void offer_(const otIp6Address &address, uint16_t port, const cJSON *request_id);
  void process_(const Packet &packet);
  bool acknowledge_report_(Node &node, const Packet &packet, const cJSON *json, bool &allow_older);
  bool parse_entity_(const cJSON *json, Entity &entity, std::string &error) const;
  bool apply_manifest_(Node &node, const cJSON *json);
  bool check_value_(const Entity &entity, const cJSON *value, bool command = false) const;
  void update_value_(Node &node, Entity &entity, const cJSON *value, bool notify_report = false);
  void queue_report_event_(const Node &node, const Entity &entity, const cJSON *value);
  void dispatch_report_events_();
  void service_commands_();
  void load_registry_();
  bool save_registry_();
  void register_entities_();
  cJSON *serialize_registry_() const;
  bool approved_fit_(std::string &error) const;
  bool metadata_supported_(const Entity &entity) const;
  uint32_t fields_(const Entity &entity) const;
  uint8_t metadata_index_(uint8_t category, const std::string &value) const;
  bool controllable_(const Node &node) const;
  bool offline_(const Node &node) const;
  Node *find_node_(const std::string &id);
  Entity *find_entity_(Node &node, const std::string &id);
  LiveEntity *find_live_(const std::string &node, const std::string &id);
  cJSON *node_json_(const Node &node, bool details) const;
  static cJSON *web_get_bridge_(void *, const char *, int *);
  static cJSON *web_post_bridge_(void *, const char *, const cJSON *, int *);

  thread_br::ThreadBorderRouter *parent_{nullptr};
  std::string gateway_id_, group_{"ff13::8000:8562"}, address_, registry_error_;
  uint16_t discovery_port_{18512}, data_port_{18513};
  uint32_t address_ttl_{3600}, announce_ms_{60000}, last_announce_{0}, last_init_{0};
  uint8_t reed_heartbeat_misses_{3};
  uint32_t restart_at_{0}, received_{0}, errors_{0}, command_id_{0};
  std::atomic<uint32_t> dropped_{0};
  bool auto_register_{false}, transport_ready_{false}, attached_{false}, dirty_{false};
  std::atomic<bool> shutting_down_{false};
  bool reboot_pending_{false};
  std::array<uint16_t, 14> capacity_{32, 16, 16};
  size_t registry_limit_{300000};
  size_t registry_bytes_{0}, registry_saved_bytes_{0};
  std::array<std::vector<std::pair<std::string, uint8_t>>, 3> metadata_;
  std::vector<Node> nodes_;
  std::vector<std::unique_ptr<LiveDevice>> live_devices_;
  std::vector<std::unique_ptr<LiveEntity>> live_;
  std::vector<Command> commands_;
  std::vector<ReportRoute<float>> sensor_report_routes_;
  std::vector<ReportRoute<bool>> binary_report_routes_;
  std::vector<ReportRoute<std::string>> text_report_routes_;
  std::vector<ReportEvent> report_events_;
  SemaphoreHandle_t mutex_{nullptr};
  QueueHandle_t packets_{nullptr};
  otUdpSocket discovery_socket_{}, data_socket_{};
};
}  // namespace esphome::thread_gateway
#endif
