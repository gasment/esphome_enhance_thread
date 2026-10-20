#include "thread_gateway.h"
#ifdef USE_THREAD_GATEWAY
#include <algorithm>
#include <cmath>
#include <cstring>
#include <new>
#include <limits>
#include <set>
#include "esphome/core/application.h"
#include "esphome/core/helpers.h"
#include "esphome/core/alloc_helpers.h"
#include "esphome/core/log.h"
#include "esphome/core/controller_registry.h"
#include "esphome/components/thread_br/thread_br.h"
#include "thread_br_web.h"
#include "esp_openthread.h"
#include "esp_openthread_lock.h"
#include "esp_random.h"
#include "nvs.h"
#include "esp_partition.h"
#include "openthread/ip6.h"
#include "openthread/message.h"
#include "openthread/thread.h"

namespace esphome::thread_gateway {
static const char *const TAG = "thread_gateway";
static constexpr size_t MAX_NODES = 128;
static constexpr size_t MAX_ENTITIES = 2048;
static constexpr size_t MAX_PER_NODE = 128;
// Read existing registries even if the configured write budget was reduced.
static constexpr size_t MAX_REGISTRY_READ = 508000;
static constexpr uint32_t CONTROL_LEASE_MS = 30000;
// Covers 60s per attempt and 10 resends, plus scheduling margin. Tiny receipt masks only.
static constexpr uint32_t REPORT_RECEIPT_RETENTION_MS = 720000;
namespace {
using Json = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;
Json owned(cJSON *value) { return Json(value, cJSON_Delete); }
const cJSON *item(const cJSON *json, const char *key) { return cJSON_GetObjectItemCaseSensitive(json, key); }
std::string str(const cJSON *json, const char *key, const std::string &fallback = "") {
  const auto *value = item(json, key);
  return cJSON_IsString(value) && value->valuestring ? value->valuestring : fallback;
}
bool flag(const cJSON *json, const char *key) { return cJSON_IsTrue(item(json, key)); }
bool bounded_string(const cJSON *json, const char *key, size_t limit, bool required = false) {
  const auto *value = item(json, key);
  return !value ? !required : cJSON_IsString(value) && value->valuestring &&
         std::strlen(value->valuestring) <= limit && (!required || value->valuestring[0]);
}
bool identifier(const std::string &s) {
  if (s.empty() || s.size() > 32) return false;
  for (unsigned char c : s)
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
          (c >= '0' && c <= '9') || c == '_' || c == '-')) return false;
  return true;
}
bool integer(const cJSON *v, double minimum, double maximum) {
  return cJSON_IsNumber(v) && std::isfinite(v->valuedouble) &&
         v->valuedouble >= minimum && v->valuedouble <= maximum &&
         std::floor(v->valuedouble) == v->valuedouble;
}
uint32_t number(const cJSON *json, const char *key, uint32_t fallback = 0) {
  const auto *v = item(json, key);
  return integer(v, 0, UINT32_MAX) ? static_cast<uint32_t>(v->valuedouble) : fallback;
}
std::string encode(const cJSON *json) {
  char *raw = cJSON_PrintUnformatted(json);
  if (!raw) return "";
  std::string value(raw);
  cJSON_free(raw);
  return value;
}
cJSON *failure(int &status, int code, const std::string &message) {
  status = code;
  auto *json = cJSON_CreateObject();
  cJSON_AddStringToObject(json, "error", message.c_str());
  return json;
}
}  // namespace

void ThreadGateway::add_metadata(uint8_t category, const std::string &value, uint8_t index) {
  if (category < metadata_.size()) metadata_[category].emplace_back(value, index);
}
uint8_t ThreadGateway::metadata_index_(uint8_t category, const std::string &value) const {
  if (value.empty()) return 0;
  for (const auto &entry : metadata_[category]) if (entry.first == value) return entry.second;
  return 0;
}
uint32_t ThreadGateway::fields_(const Entity &e) const {
  const auto category = e.entity_category == "config" ? ENTITY_CATEGORY_CONFIG :
      (e.entity_category == "diagnostic" ? ENTITY_CATEGORY_DIAGNOSTIC : ENTITY_CATEGORY_NONE);
  return metadata_index_(0, e.device_class) | (uint32_t(metadata_index_(1, e.unit)) << 8) |
         (uint32_t(metadata_index_(2, e.icon)) << 16) |
         (uint32_t(category) << ENTITY_FIELD_ENTITY_CATEGORY_SHIFT) |
         (uint32_t(e.disabled_by_default) << ENTITY_FIELD_DISABLED_BY_DEFAULT_SHIFT);
}
bool ThreadGateway::metadata_supported_(const Entity &e) const {
  // API adaptation supplies metadata from the persisted manifest directly,
  // including strings absent from the firmware's packed local-entity tables.
  return e.device_class.size()<=64 && e.unit.size()<=64 && e.icon.size()<=64;
}
ThreadGateway::Node *ThreadGateway::find_node_(const std::string &id) {
  for (auto &node : nodes_) if (node.id == id) return &node;
  return nullptr;
}
ThreadGateway::Entity *ThreadGateway::find_entity_(Node &node, const std::string &id) {
  for (auto &entity : node.entities) if (entity.id == id) return &entity;
  return nullptr;
}
ThreadGateway::LiveEntity *ThreadGateway::find_live_(const std::string &node, const std::string &id) {
  for (auto &live : live_) if (live->node == node && live->id == id) return live.get();
  return nullptr;
}
bool ThreadGateway::offline_(const Node &node) const {
  if(node.node_type!="reed")return false;
  const uint32_t timeout=node.heartbeat_ms*reed_heartbeat_misses_;
  // A restarted gateway gives restored REED nodes a full heartbeat grace period.
  const uint32_t age=node.seen ? uint32_t(millis()-node.last_seen) : millis();
  return age>=timeout;
}
bool ThreadGateway::controllable_(const Node &node) const {
  return !offline_(node) && node.seen && node.mode == "online" && !node.session.empty() &&
         static_cast<uint32_t>(millis() - node.control_seen) < CONTROL_LEASE_MS;
}

void ThreadGateway::setup() {
  mutex_ = xSemaphoreCreateRecursiveMutex();
  packets_ = xQueueCreate(8, sizeof(Packet *));
  if (!mutex_ || !packets_) { mark_failed(); return; }
  if (gateway_id_.empty()) gateway_id_ = "br_" + get_mac_address();
  command_id_ = esp_random();
  load_registry_();
  register_entities_();
  thread_br_thread_gateway_web_t callbacks{};
  callbacks.context = this;
  callbacks.get = web_get_bridge_;
  callbacks.post = web_post_bridge_;
  thread_br_web_set_thread_gateway(&callbacks);
}
void ThreadGateway::dump_config() {
  ESP_LOGCONFIG(TAG, "Thread gateway '%s', discovery=%u data=%u TTL=%us",
                gateway_id_.c_str(), discovery_port_, data_port_, (unsigned) address_ttl_);
  ESP_LOGCONFIG(TAG, "  HA capacities: numeric=%u binary=%u text=%u; active=%u; auto-register=%s",
                capacity_[0], capacity_[1], capacity_[2], (unsigned) live_.size(), YESNO(auto_register_));
  if (!registry_error_.empty()) ESP_LOGW(TAG, "Registry: %s", registry_error_.c_str());
}

bool ThreadGateway::parse_entity_(const cJSON *json, Entity &e, std::string &error) const {
  if (!cJSON_IsObject(json) || !identifier(str(json, "id"))) { error = "Invalid entity id"; return false; }
  // Each manifest is a complete descriptor: omitted metadata gets defaults,
  // never the value from an earlier manifest or a reused Entity object.
  e = Entity{};
  for (const auto *key : {"disabled_by_default", "force_update", "trigger_on_initial_state"}) {
    const auto *value = item(json, key);
    if (value && !cJSON_IsBool(value)) { error = std::string(key) + " must be bool"; return false; }
  }
  e.disabled_by_default = flag(json, "disabled_by_default");
  e.force_update = flag(json, "force_update");
  e.trigger_on_initial_state = flag(json, "trigger_on_initial_state");
  for (const auto *key : {"name", "unit", "device_class", "icon", "state_class", "entity_category"})
    if (!bounded_string(json, key, std::strcmp(key, "name") == 0 ? 96 : 64)) {
      error = "Invalid entity metadata string"; return false;
    }
  e.id = str(json, "id"); e.name = str(json, "name", e.id);
  const std::string type = str(json, "type");
  if (type == "numeric" || type == "sensor") e.kind = Kind::NUMERIC;
  else if (type == "binary" || type == "binary_sensor") e.kind = Kind::BINARY;
  else if (type == "text" || type == "text_sensor") e.kind = Kind::TEXT;
  else if (type == "switch") e.kind = Kind::SWITCH;
  else if (type == "number") e.kind = Kind::NUMBER;
  else if (type == "select") e.kind = Kind::SELECT;
  else if (type == "button") e.kind = Kind::BUTTON;
  else if (type == "input_text") e.kind = Kind::INPUT_TEXT;
  else if (type == "light") e.kind = Kind::LIGHT;
  else if (type == "fan") e.kind = Kind::FAN;
  else if (type == "cover") e.kind = Kind::COVER;
  else if (type == "climate") e.kind = Kind::CLIMATE;
  else if (type == "lock") e.kind = Kind::LOCK;
  else if (type == "valve") e.kind = Kind::VALVE;
  else { error = "Unsupported entity type"; return false; }
  if (const auto *caps = item(json, "capabilities")) {
    if (!thread_entity_bridge::validate_capabilities(type, caps)) { error="Invalid capabilities"; return false; }
    e.capabilities=encode(caps);
  }
  if (e.kind == Kind::TEXT) e.max_length = 255;
  e.unit = str(json, "unit"); e.device_class = str(json, "device_class"); e.icon = str(json, "icon");
  e.state_class = str(json, "state_class");
  e.entity_category = str(json, "entity_category");
  if (e.entity_category == "none") e.entity_category.clear();
  if (!e.entity_category.empty() && e.entity_category != "config" && e.entity_category != "diagnostic") {
    error = "entity_category must be empty, config or diagnostic"; return false;
  }
  if (!e.state_class.empty() && e.state_class != "measurement" && e.state_class != "total" &&
      e.state_class != "total_increasing" && e.state_class != "measurement_angle") { error = "Invalid state_class"; return false; }
  if (const auto *accuracy = item(json, "accuracy_decimals")) {
    if (!integer(accuracy, -6, 6)) { error = "accuracy_decimals must be -6..6"; return false; }
    e.decimals = static_cast<int8_t>(accuracy->valuedouble);
  }
  if (e.kind == Kind::NUMBER) {
    const auto *lo = item(json, "min"), *hi = item(json, "max"), *step = item(json, "step");
    if (!cJSON_IsNumber(lo) || !cJSON_IsNumber(hi) || !cJSON_IsNumber(step) ||
        !std::isfinite(lo->valuedouble) || !std::isfinite(hi->valuedouble) ||
        !std::isfinite(step->valuedouble) || lo->valuedouble > hi->valuedouble ||
        step->valuedouble <= 0) { error = "Invalid number min/max/step"; return false; }
    e.minimum = lo->valuedouble; e.maximum = hi->valuedouble; e.step = step->valuedouble;
  }
  if (e.kind == Kind::SELECT) {
    const auto *options = item(json, "options");
    if (!cJSON_IsArray(options) || cJSON_GetArraySize(options) < 1 || cJSON_GetArraySize(options) > 32) {
      error = "select requires 1..32 options"; return false;
    }
    const cJSON *option;
    cJSON_ArrayForEach(option, options) {
      if (!cJSON_IsString(option) || !option->valuestring || std::strlen(option->valuestring) > 64 ||
          std::find(e.options.begin(), e.options.end(), option->valuestring) != e.options.end()) {
        error = "Invalid/duplicate select option"; return false;
      }
      e.options.emplace_back(option->valuestring);
    }
  }
  if (const auto *length = item(json, "max_length")) {
    if (!integer(length, 1, 255)) { error = "max_length must be 1..255 UTF-8 bytes"; return false; }
    e.max_length = static_cast<uint16_t>(length->valuedouble);
  }
  return true;
}
bool ThreadGateway::check_value_(const Entity &e, const cJSON *v, bool command) const {
  if (!e.capabilities.empty()) {
    auto caps=owned(cJSON_Parse(e.capabilities.c_str()));
    static const char *types[] = {"numeric", "binary", "text", "switch", "number", "select", "button", "input_text", "light", "fan", "cover", "climate", "lock", "valve"};
    return thread_entity_bridge::validate_value(types[static_cast<unsigned>(e.kind)], caps.get(), v, command);
  }
  if (e.kind == Kind::BUTTON) return !v || cJSON_IsNull(v);
  if (e.kind == Kind::NUMERIC || e.kind == Kind::NUMBER) {
    if (!cJSON_IsNumber(v) || !std::isfinite(v->valuedouble)) return false;
    if (e.kind == Kind::NUMERIC) return std::abs(v->valuedouble) <= std::numeric_limits<float>::max();
    if (!command) return true;
    const double steps = (v->valuedouble - e.minimum) / e.step;
    return v->valuedouble >= e.minimum && v->valuedouble <= e.maximum &&
           std::abs(steps - std::round(steps)) <= 1e-6 * std::max(1.0, std::abs(steps));
  }
  if (e.kind == Kind::BINARY || e.kind == Kind::SWITCH) return cJSON_IsBool(v);
  if (!cJSON_IsString(v) || !v->valuestring || std::strlen(v->valuestring) >
      e.max_length) return false;
  return e.kind != Kind::SELECT ||
         std::find(e.options.begin(), e.options.end(), v->valuestring) != e.options.end();
}
void ThreadGateway::update_value_(Node &node, Entity &e, const cJSON *value, bool notify_report) {
  if (!check_value_(e, value)) { node.errors++; node.last_error = "Invalid value for " + e.id; return; }
  e.value = encode(value); e.has_value = true; e.value_at = millis();
  if (notify_report) queue_report_event_(node, e, value);
  auto *live = find_live_(node.id, e.id);
  if (!live || live->kind != e.kind) return;
  EntityBase *source=live->numeric ? static_cast<EntityBase *>(live->numeric) : live->binary ? static_cast<EntityBase *>(live->binary) : live->text ? static_cast<EntityBase *>(live->text) : live->proxy ? live->proxy->entity : nullptr;
  if(source)thread_entity_bridge::mark_remote_ready(source);
  if (live->numeric) live->numeric->publish_state(static_cast<float>(value->valuedouble));
  else if (live->binary) live->binary->publish_state(cJSON_IsTrue(value));
  else if (live->text) live->text->publish_state(value->valuestring);
  else if (live->proxy && live->proxy->apply) live->proxy->apply(value);
  // The initial API iterator may already have skipped an unready proxy.
  // After a new session, explicitly deliver its first authoritative snapshot,
  // even if the native publisher deduplicates an unchanged value.
#ifdef USE_CONTROLLER_REGISTRY
  if (live->notify_on_sync && live->proxy) {
    switch (live->kind) {
#ifdef USE_SWITCH
      case Kind::SWITCH:
        ControllerRegistry::notify_switch_update(static_cast<switch_::Switch *>(live->proxy->entity));
        break;
#endif
#ifdef USE_NUMBER
      case Kind::NUMBER:
        ControllerRegistry::notify_number_update(static_cast<number::Number *>(live->proxy->entity));
        break;
#endif
#ifdef USE_SELECT
      case Kind::SELECT:
        ControllerRegistry::notify_select_update(static_cast<select::Select *>(live->proxy->entity));
        break;
#endif
#ifdef USE_TEXT
      case Kind::INPUT_TEXT:
        ControllerRegistry::notify_text_update(static_cast<text::Text *>(live->proxy->entity));
        break;
#endif
#ifdef USE_LIGHT
      case Kind::LIGHT:
        ControllerRegistry::notify_light_update(static_cast<light::LightState *>(live->proxy->entity));
        break;
#endif
#ifdef USE_FAN
      case Kind::FAN:
        ControllerRegistry::notify_fan_update(static_cast<fan::Fan *>(live->proxy->entity));
        break;
#endif
#ifdef USE_COVER
      case Kind::COVER:
        ControllerRegistry::notify_cover_update(static_cast<cover::Cover *>(live->proxy->entity));
        break;
#endif
#ifdef USE_CLIMATE
      case Kind::CLIMATE:
        ControllerRegistry::notify_climate_update(static_cast<climate::Climate *>(live->proxy->entity));
        break;
#endif
#ifdef USE_LOCK
      case Kind::LOCK:
        ControllerRegistry::notify_lock_update(static_cast<lock::Lock *>(live->proxy->entity));
        break;
#endif
#ifdef USE_VALVE
      case Kind::VALVE:
        ControllerRegistry::notify_valve_update(static_cast<valve::Valve *>(live->proxy->entity));
        break;
#endif
      default: break;
    }
  }
#endif
  live->notify_on_sync = false;
}

void ThreadGateway::queue_report_event_(const Node &node, const Entity &entity, const cJSON *value) {
  // Match first: unconfigured entities allocate no callback event.
  const auto matches = [&node, &entity](const auto &routes) {
    return std::any_of(routes.begin(), routes.end(), [&node, &entity](const auto &route) {
      return route.node == node.id && route.entity == entity.id;
    });
  };
  ReportEvent event;
  event.kind = entity.kind;
  if (entity.kind == Kind::NUMERIC && matches(sensor_report_routes_))
    event.number = static_cast<float>(value->valuedouble);
  else if (entity.kind == Kind::BINARY && matches(binary_report_routes_))
    event.binary = cJSON_IsTrue(value);
  else if (entity.kind == Kind::TEXT && matches(text_report_routes_))
    event.text = value->valuestring;
  else return;
  event.node = node.id; event.entity = entity.id;
  report_events_.push_back(std::move(event));
}

void ThreadGateway::dispatch_report_events_() {
  // Own each value: no references to JSON, node vectors, or packet buffers escape.
  // Run user automations on the main loop with the registry mutex released.
  auto events = std::move(report_events_);
  report_events_.clear();
  for (const auto &event : events) {
    const auto dispatch = [&event](const auto &routes, const auto &value) {
      for (const auto &route : routes)
        if (route.node == event.node && route.entity == event.entity) route.callback(value);
    };
    if (event.kind == Kind::NUMERIC) dispatch(sensor_report_routes_, event.number);
    else if (event.kind == Kind::BINARY) dispatch(binary_report_routes_, event.binary);
    else if (event.kind == Kind::TEXT) dispatch(text_report_routes_, event.text);
  }
}

bool ThreadGateway::approved_fit_(std::string &error) const {
  std::array<unsigned, 14> count{};
  std::set<uint32_t> devices{0};
  for (auto *device : App.get_devices()) devices.insert(device->get_device_id());
  for (const auto &live : live_devices_) devices.erase(live->device.get_device_id());
  for (const auto &node : nodes_) {
    if (std::none_of(node.entities.begin(), node.entities.end(), [](const Entity &e) { return e.approved; })) continue;
    const uint32_t id = fnv1a_hash("thread_gateway_" + node.id);
    if (!devices.insert(id).second) { error = "Sub-device identity collision for " + node.id; return false; }
  }
  std::set<uint32_t> hashes;
  // Include existing local entities, so collisions never silently alias API keys.
  for (auto *entity : App.get_sensors()) hashes.insert(entity->get_object_id_hash());
  for (auto *entity : App.get_binary_sensors()) hashes.insert(entity->get_object_id_hash());
  for (auto *entity : App.get_text_sensors()) hashes.insert(entity->get_object_id_hash());
#ifdef USE_SWITCH
  for (auto *entity : App.get_switches()) hashes.insert(entity->get_object_id_hash());
#endif
#ifdef USE_NUMBER
  for (auto *entity : App.get_numbers()) hashes.insert(entity->get_object_id_hash());
#endif
#ifdef USE_SELECT
  for (auto *entity : App.get_selects()) hashes.insert(entity->get_object_id_hash());
#endif
#ifdef USE_BUTTON
  for (auto *entity : App.get_buttons()) hashes.insert(entity->get_object_id_hash());
#endif
#ifdef USE_TEXT
  for (auto *entity : App.get_texts()) hashes.insert(entity->get_object_id_hash());
#endif
#ifdef USE_LIGHT
  for (auto *entity : App.get_lights()) hashes.insert(entity->get_object_id_hash());
#endif
#ifdef USE_FAN
  for (auto *entity : App.get_fans()) hashes.insert(entity->get_object_id_hash());
#endif
#ifdef USE_COVER
  for (auto *entity : App.get_covers()) hashes.insert(entity->get_object_id_hash());
#endif
#ifdef USE_CLIMATE
  for (auto *entity : App.get_climates()) hashes.insert(entity->get_object_id_hash());
#endif
#ifdef USE_LOCK
  for (auto *entity : App.get_locks()) hashes.insert(entity->get_object_id_hash());
#endif
#ifdef USE_VALVE
  for (auto *entity : App.get_valves()) hashes.insert(entity->get_object_id_hash());
#endif
  // Own boot-registered entries are already in App. Allow exactly that same identity.
  for (const auto &live : live_) hashes.erase(fnv1_hash_object_id(live->name.c_str(), live->name.size()));
  for (const auto &node : nodes_) for (const auto &e : node.entities) {
    if (!e.approved) continue;
    const auto kind = static_cast<unsigned>(e.kind);
    if (kind > 2 && !(node.node_type=="reed")) { error = "SED controls remain Web-only"; return false; }
    if (++count[kind] > capacity_[kind]) { error = "Entity type capacity exceeded"; return false; }
    if (!metadata_supported_(e)) { error = "Unsupported unit/device_class/icon for " + node.id + "/" + e.id; return false; }
    const std::string name = "thread_gateway_" + node.id + "_" + e.id;
    if (!hashes.insert(fnv1_hash_object_id(name.c_str(), name.size())).second) {
      error = "Entity identity/key collision"; return false;
    }
  }
  return true;
}
cJSON *ThreadGateway::serialize_registry_() const {
  auto *root = cJSON_CreateObject();
  cJSON_AddNumberToObject(root, "version", 1);
  auto *nodes = cJSON_AddArrayToObject(root, "nodes");
  static const char *types[] = {"numeric", "binary", "text", "switch", "number", "select", "button", "input_text", "light", "fan", "cover", "climate", "lock", "valve"};
  for (const auto &node : nodes_) {
    auto *n = cJSON_CreateObject(); cJSON_AddItemToArray(nodes, n);
    cJSON_AddStringToObject(n, "id", node.id.c_str()); cJSON_AddStringToObject(n, "name", node.name.c_str());
    cJSON_AddNumberToObject(n, "revision", node.revision);
    cJSON_AddStringToObject(n,"node_type",node.node_type.c_str());
    if(node.node_type=="reed") {
      cJSON_AddNumberToObject(n,"heartbeat_interval_ms",node.heartbeat_ms);
    }
    auto *entities = cJSON_AddArrayToObject(n, "entities");
    for (const auto &e : node.entities) {
      auto *j = cJSON_CreateObject(); cJSON_AddItemToArray(entities, j);
      cJSON_AddStringToObject(j, "id", e.id.c_str()); cJSON_AddStringToObject(j, "name", e.name.c_str());
      cJSON_AddStringToObject(j, "type", types[static_cast<unsigned>(e.kind)]);
      if(!e.capabilities.empty()) cJSON_AddItemToObject(j,"capabilities",cJSON_Parse(e.capabilities.c_str()));
      // Missing fields are restored by parse_entity_ defaults; retain non-default values.
      if (!e.unit.empty()) cJSON_AddStringToObject(j, "unit", e.unit.c_str());
      if (!e.device_class.empty()) cJSON_AddStringToObject(j, "device_class", e.device_class.c_str());
      if (!e.icon.empty()) cJSON_AddStringToObject(j, "icon", e.icon.c_str());
      if (!e.state_class.empty()) cJSON_AddStringToObject(j, "state_class", e.state_class.c_str());
      if (!e.entity_category.empty()) cJSON_AddStringToObject(j, "entity_category", e.entity_category.c_str());
      if (e.decimals != 0) cJSON_AddNumberToObject(j, "accuracy_decimals", e.decimals);
      if (e.approved) cJSON_AddBoolToObject(j, "approved", true);
      if (e.disabled_by_default) cJSON_AddBoolToObject(j, "disabled_by_default", true);
      if (e.force_update) cJSON_AddBoolToObject(j, "force_update", true);
      if (e.trigger_on_initial_state) cJSON_AddBoolToObject(j, "trigger_on_initial_state", true);
      if (e.kind == Kind::NUMBER) {
        cJSON_AddNumberToObject(j, "min", e.minimum); cJSON_AddNumberToObject(j, "max", e.maximum);
        cJSON_AddNumberToObject(j, "step", e.step);
      }
      if (e.max_length != (e.kind == Kind::TEXT ? 255 : 128))
        cJSON_AddNumberToObject(j, "max_length", e.max_length);
      if (e.kind == Kind::SELECT) {
        auto *options = cJSON_AddArrayToObject(j, "options");
        for (const auto &option : e.options) cJSON_AddItemToArray(options, cJSON_CreateString(option.c_str()));
      }
    }
  }
  return root;
}
bool ThreadGateway::save_registry_() {
  auto json = owned(serialize_registry_());
  const std::string blob = encode(json.get());
  json.reset();  // Do not retain the JSON tree while NVS copies a large registry.
  registry_bytes_ = blob.size() + 1;
  if (blob.empty() || registry_bytes_ > registry_limit_) {
    registry_error_ = "Registry needs " + std::to_string(registry_bytes_) +
                      " bytes; capacity.nvs limit is " + std::to_string(registry_limit_) + " bytes";
    ESP_LOGW(TAG, "%s", registry_error_.c_str());
    return false;
  }
  nvs_handle_t nvs;
  esp_err_t err = nvs_open("thread_gateway", NVS_READWRITE, &nvs);
  if (err == ESP_OK) {
    err = nvs_set_blob(nvs, "registry", blob.c_str(), blob.size() + 1);
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
  }
  if (err != ESP_OK) {
    registry_error_ = std::string("NVS save failed: ") + esp_err_to_name(err);
    ESP_LOGW(TAG, "%s", registry_error_.c_str()); return false;
  }
  registry_saved_bytes_ = registry_bytes_;
  registry_error_.clear();
  return true;
}
void ThreadGateway::load_registry_() {
  nvs_handle_t nvs;
  auto err = nvs_open("thread_gateway", NVS_READONLY, &nvs);
  if (err == ESP_ERR_NVS_NOT_FOUND) return;
  if (err != ESP_OK) { registry_error_ = esp_err_to_name(err); return; }
  size_t length = 0;
  err = nvs_get_blob(nvs, "registry", nullptr, &length);
  if (err == ESP_ERR_NVS_NOT_FOUND) { nvs_close(nvs); return; }
  if (err != ESP_OK || length < 2 || length > MAX_REGISTRY_READ) {
    nvs_close(nvs); registry_error_ = "Invalid NVS registry size"; return;
  }
  std::vector<char> data(length);
  err = nvs_get_blob(nvs, "registry", data.data(), &length);
  nvs_close(nvs);
  if (err != ESP_OK || data.back() != 0) { registry_error_ = "Cannot read NVS registry"; return; }
  registry_saved_bytes_ = length;
  auto root = owned(cJSON_Parse(data.data()));
  const auto *list = item(root.get(), "nodes");
  bool valid = number(root.get(), "version") == 1 && cJSON_IsArray(list) &&
               cJSON_GetArraySize(list) <= static_cast<int>(MAX_NODES);
  size_t total = 0;
  std::set<std::string> node_ids;
  if (valid) {
    const cJSON *n;
    cJSON_ArrayForEach(n, list) {
      Node node; node.id = str(n, "id"); node.name = str(n, "name", node.id); node.revision = number(n, "revision"); node.node_type=str(n,"node_type");
      if(node.node_type!="reed" && node.node_type!="sed"){valid=false;break;}
      node.heartbeat_ms=number(n,"heartbeat_interval_ms",10000);
      if(node.heartbeat_ms<1000||node.heartbeat_ms>15000){valid=false;break;}
      const auto *entities = item(n, "entities");
      if (!identifier(node.id) || !node_ids.insert(node.id).second ||
          !bounded_string(n, "name", 96) || !cJSON_IsArray(entities) ||
          cJSON_GetArraySize(entities) > static_cast<int>(MAX_PER_NODE)) { valid = false; break; }
      std::set<std::string> ids;
      const cJSON *j;
      cJSON_ArrayForEach(j, entities) {
        Entity entity; std::string error;
        if (!parse_entity_(j, entity, error) || !ids.insert(entity.id).second || ++total > MAX_ENTITIES) {
          valid = false; break;
        }
        entity.approved = flag(j, "approved");
        if (entity.approved && static_cast<unsigned>(entity.kind) > 2 && !(node.node_type=="reed")) { valid = false; break; }
        node.entities.push_back(std::move(entity));
      }
      if (!valid) break;
      nodes_.push_back(std::move(node));
    }
  }
  if (!valid) { nodes_.clear(); registry_error_ = "Invalid NVS registry; no remote entities registered"; }
  else {
    auto compact = owned(serialize_registry_());
    registry_bytes_ = encode(compact.get()).size() + 1;
    if (registry_bytes_ > registry_limit_)
      registry_error_ = "Existing registry exceeds capacity.nvs; retained, increase limit or remove nodes before saving";
  }
}
void ThreadGateway::register_entities_() {
  std::string error;
  if (!approved_fit_(error)) { registry_error_ = error; return; }
  for (const auto &node : nodes_) {
    if (std::none_of(node.entities.begin(), node.entities.end(), [](const Entity &e) { return e.approved; })) continue;
    auto device = std::make_unique<LiveDevice>();
    device->node = node.id;
    device->name = node.name.empty() ? node.id : node.name;
    device->device.set_device_id(fnv1a_hash("thread_gateway_" + node.id));
    device->device.set_name(device->name.c_str());
    auto *device_ptr = &device->device;
    App.register_device(device_ptr);
    // Own the name independently from Node: deletion/manifest edits must not invalidate API pointers before reboot.
    live_devices_.push_back(std::move(device));
    for (const auto &e : node.entities) {
      if (!e.approved) continue;
      auto live = std::make_unique<LiveEntity>();
      live->node = node.id; live->id = e.id; live->kind = e.kind;
      // Stable protocol key; user-facing names may change without changing node/entity identities.
      live->name = "thread_gateway_" + node.id + "_" + e.id;
      live->display_name = e.name.empty() ? e.id : e.name;
      const auto hash = fnv1_hash_object_id(live->name.c_str(), live->name.size());
      if (e.kind == Kind::NUMERIC) {
        live->numeric = new RemoteSensor();
        live->numeric->set_node_device(device_ptr);
        live->numeric->set_accuracy_decimals(e.decimals);
        live->numeric->set_force_update(e.force_update);
        auto state_class = sensor::STATE_CLASS_NONE;
        if (e.state_class == "measurement") state_class = sensor::STATE_CLASS_MEASUREMENT;
        else if (e.state_class == "total") state_class = sensor::STATE_CLASS_TOTAL;
        else if (e.state_class == "total_increasing") state_class = sensor::STATE_CLASS_TOTAL_INCREASING;
        else if (e.state_class == "measurement_angle") state_class = sensor::STATE_CLASS_MEASUREMENT_ANGLE;
        live->numeric->set_state_class(state_class);
        App.register_sensor(live->numeric, live->display_name.c_str(), hash, fields_(e));
      } else if (e.kind == Kind::BINARY) {
        live->binary = new RemoteBinary();
        live->binary->set_node_device(device_ptr);
        live->binary->set_trigger_on_initial_state(e.trigger_on_initial_state);
        App.register_binary_sensor(live->binary, live->display_name.c_str(), hash, fields_(e));
      } else if (e.kind == Kind::TEXT) {
        live->text = new RemoteText();
        live->text->set_node_device(device_ptr);
        App.register_text_sensor(live->text, live->display_name.c_str(), hash, fields_(e));
      }
      if (static_cast<unsigned>(e.kind)>2) {
        auto caps=owned(cJSON_Parse(e.capabilities.empty()?"{}":e.capabilities.c_str()));
        // Legacy control descriptors have flat traits.
        if(e.capabilities.empty()) {
          cJSON_AddNumberToObject(caps.get(),"min",e.minimum); cJSON_AddNumberToObject(caps.get(),"max",e.maximum); cJSON_AddNumberToObject(caps.get(),"step",e.step);
          cJSON_AddNumberToObject(caps.get(),"max_length",e.max_length);
          auto *options=cJSON_AddArrayToObject(caps.get(),"options");for(auto &option:e.options)cJSON_AddItemToArray(options,cJSON_CreateString(option.c_str()));
        }
        static const char *types[] = {"numeric", "binary", "text", "switch", "number", "select", "button", "input_text", "light", "fan", "cover", "climate", "lock", "valve"};
        const std::string node_id=node.id, entity_id=e.id;
        live->proxy=thread_entity_bridge::make_proxy(types[static_cast<unsigned>(e.kind)],caps.get(),
          [this,node_id,entity_id](const cJSON *v){send_control(node_id,entity_id,v);});
        if(!live->proxy) {registry_error_="Control type not enabled in capacity";continue;}
        live->proxy->set_device(device_ptr);
#ifdef USE_SWITCH
        if(e.kind==Kind::SWITCH) App.register_switch(static_cast<switch_::Switch *>(live->proxy->entity),live->display_name.c_str(),hash,fields_(e));
#endif
#ifdef USE_NUMBER
        if(e.kind==Kind::NUMBER) App.register_number(static_cast<number::Number *>(live->proxy->entity),live->display_name.c_str(),hash,fields_(e));
#endif
#ifdef USE_SELECT
        if(e.kind==Kind::SELECT) App.register_select(static_cast<select::Select *>(live->proxy->entity),live->display_name.c_str(),hash,fields_(e));
#endif
#ifdef USE_BUTTON
        if(e.kind==Kind::BUTTON) App.register_button(static_cast<button::Button *>(live->proxy->entity),live->display_name.c_str(),hash,fields_(e));
#endif
#ifdef USE_TEXT
        if(e.kind==Kind::INPUT_TEXT) App.register_text(static_cast<text::Text *>(live->proxy->entity),live->display_name.c_str(),hash,fields_(e));
#endif
#ifdef USE_LIGHT
        if(e.kind==Kind::LIGHT) App.register_light(static_cast<light::LightState *>(live->proxy->entity),live->display_name.c_str(),hash,fields_(e));
#endif
#ifdef USE_FAN
        if(e.kind==Kind::FAN) App.register_fan(static_cast<fan::Fan *>(live->proxy->entity),live->display_name.c_str(),hash,fields_(e));
#endif
#ifdef USE_COVER
        if(e.kind==Kind::COVER) App.register_cover(static_cast<cover::Cover *>(live->proxy->entity),live->display_name.c_str(),hash,fields_(e));
#endif
#ifdef USE_CLIMATE
        if(e.kind==Kind::CLIMATE) App.register_climate(static_cast<climate::Climate *>(live->proxy->entity),live->display_name.c_str(),hash,fields_(e));
#endif
#ifdef USE_LOCK
        if(e.kind==Kind::LOCK) App.register_lock(static_cast<lock::Lock *>(live->proxy->entity),live->display_name.c_str(),hash,fields_(e));
#endif
#ifdef USE_VALVE
        if(e.kind==Kind::VALVE) App.register_valve(static_cast<valve::Valve *>(live->proxy->entity),live->display_name.c_str(),hash,fields_(e));
#endif
      }
      EntityBase *registered=live->numeric ? static_cast<EntityBase *>(live->numeric) : live->binary ? static_cast<EntityBase *>(live->binary) : live->text ? static_cast<EntityBase *>(live->text) : live->proxy ? live->proxy->entity : nullptr;
      if(registered)thread_entity_bridge::register_remote(registered,e.device_class,e.unit,e.icon);
      live_.push_back(std::move(live));
    }
  }
}

bool ThreadGateway::apply_manifest_(Node &node, const cJSON *json) {
  const auto *revision = item(json, "revision"), *part = item(json, "part"), *parts = item(json, "parts");
  const auto *entities = item(json, "entities");
  if (!integer(revision, 1, UINT32_MAX) || !integer(part, 0, 15) || !integer(parts, 1, 16) ||
      part->valuedouble >= parts->valuedouble || !cJSON_IsArray(entities) ||
      cJSON_GetArraySize(entities) > 32) { node.last_error = "Invalid manifest revision/part/entities"; return false; }
  const auto rev = static_cast<uint32_t>(revision->valuedouble);
  const unsigned index = static_cast<unsigned>(part->valuedouble), count = static_cast<unsigned>(parts->valuedouble);
  auto &m = node.manifest;
  if (m.parts && static_cast<uint32_t>(millis() - m.started_at) > 15000) m = Manifest{};
  if (node.revision && rev < node.revision) { node.last_error = "Stale manifest revision"; return false; }
  if (m.parts == 0 || m.revision != rev) {
    m = Manifest{}; m.revision = rev; m.parts = count; m.started_at = millis();
    m.name = str(json, "name", node.name);
    m.received.assign(count, false); m.pages.resize(count);
  }
  if (m.parts != count || m.name != str(json, "name", node.name)) {
    node.last_error = "Manifest part count/name changed"; return false;
  }
  std::vector<Entity> page;
  const cJSON *j;
  cJSON_ArrayForEach(j, entities) {
    Entity e; std::string error;
    if (!parse_entity_(j, e, error)) { node.last_error = error; return false; }
    page.push_back(std::move(e));
  }
  m.pages[index] = std::move(page); m.received[index] = true;
  for (bool done : m.received) if (!done) return true;
  std::vector<Entity> complete;
  std::set<std::string> ids;
  size_t other = 0;
  for (const auto &n : nodes_) if (n.id != node.id) other += n.entities.size();
  for (auto &p : m.pages) for (auto &e : p) {
    if (!ids.insert(e.id).second || complete.size() >= MAX_PER_NODE || other + complete.size() >= MAX_ENTITIES) {
      node.last_error = "Duplicate entity or registry limit exceeded"; m = Manifest{}; return false;
    }
    if (auto *old = find_entity_(node, e.id); old && old->kind == e.kind) {
      e.approved = old->approved; e.value = old->value; e.has_value = old->has_value; e.value_at = old->value_at; e.source_sequence=old->source_sequence; e.has_source_sequence=old->has_source_sequence;
    }
    complete.push_back(std::move(e));
  }
  if (node.revision == rev) {
    bool same = complete.size() == node.entities.size();
    for (const auto &e : complete) {
      const auto *old = find_entity_(node, e.id);
      same &= old && old->kind == e.kind && old->name == e.name && old->unit == e.unit &&
          old->device_class == e.device_class && old->icon == e.icon && old->state_class == e.state_class &&
          old->entity_category == e.entity_category && old->capabilities == e.capabilities &&
          old->disabled_by_default == e.disabled_by_default && old->force_update == e.force_update &&
          old->trigger_on_initial_state == e.trigger_on_initial_state &&
          old->decimals == e.decimals && old->minimum == e.minimum && old->maximum == e.maximum &&
          old->step == e.step && old->max_length == e.max_length && old->options == e.options;
    }
    if (!same || node.name != m.name) {
      node.last_error = "Schema changed without increasing revision"; m = Manifest{}; return false;
    }
  }
  // Unchanged full manifests never cause NVS writes. Revisions must increase for schema changes.
  auto before = owned(serialize_registry_());
  const std::string previous = encode(before.get());
  before.reset();
  auto old = std::move(node.entities);
  const auto old_revision = node.revision;
  const auto old_name = node.name; node.name = m.name;
  node.entities = std::move(complete); node.revision = rev; m = Manifest{};
  if (auto_register_) {
    for (auto &e : node.entities) {
      if (e.approved || (static_cast<unsigned>(e.kind) > 2 && !(node.node_type=="reed")) || !metadata_supported_(e)) continue;
      e.approved = true; std::string error;
      if (!approved_fit_(error)) e.approved = false;
    }
  }
  std::string validation;
  if (!approved_fit_(validation)) {
    node.entities = std::move(old); node.revision = old_revision; node.name = old_name; node.last_error = validation; return false;
  }
  auto after = owned(serialize_registry_());
  const std::string current = encode(after.get());
  after.reset();
  if (current != previous) {
    dirty_ = true;
    if (!save_registry_()) node.last_error = registry_error_;
  }
  return true;
}

void ThreadGateway::receive_discovery_(void *ctx, otMessage *msg, const otMessageInfo *info) {
  static_cast<ThreadGateway *>(ctx)->receive_(true, msg, info);
}
void ThreadGateway::receive_data_(void *ctx, otMessage *msg, const otMessageInfo *info) {
  static_cast<ThreadGateway *>(ctx)->receive_(false, msg, info);
}
void ThreadGateway::receive_(bool discovery, otMessage *message, const otMessageInfo *info) {
  if (shutting_down_ || !packets_) return;
  const auto length = otMessageGetLength(message) - otMessageGetOffset(message);
  if (length <= 0 || length > 1232) { dropped_++; return; }
  auto *packet = new (std::nothrow) Packet;
  if (!packet) { dropped_++; return; }
  packet->address = info->mPeerAddr; packet->port = info->mPeerPort;
  packet->discovery = discovery; packet->size = length;
  if (otMessageRead(message, otMessageGetOffset(message), packet->data, length) != length) {
    delete packet; dropped_++; return;
  }
  packet->data[length] = 0;
  if (xQueueSend(packets_, &packet, 0) != pdTRUE) { delete packet; dropped_++; }
}
bool ThreadGateway::start_transport_() {
  if (!parent_ || !parent_->is_thread_ready()) return false;
  if (!esp_openthread_lock_acquire(0)) return false;
  auto *ot = esp_openthread_get_instance();
  bool ready = false;
  if (ot && otThreadGetDeviceRole(ot) >= OT_DEVICE_ROLE_CHILD) {
    otIp6Address group{};
    otSockAddr discovery{}, data{};
    discovery.mPort = discovery_port_; data.mPort = data_port_;
    if (otIp6AddressFromString(group_.c_str(), &group) == OT_ERROR_NONE &&
        otUdpOpen(ot, &discovery_socket_, receive_discovery_, this) == OT_ERROR_NONE) {
      if (otUdpBind(ot, &discovery_socket_, &discovery, OT_NETIF_THREAD_INTERNAL) == OT_ERROR_NONE &&
          otUdpOpen(ot, &data_socket_, receive_data_, this) == OT_ERROR_NONE) {
        const auto join = otIp6SubscribeMulticastAddress(ot, &group);
        ready = otUdpBind(ot, &data_socket_, &data, OT_NETIF_THREAD_INTERNAL) == OT_ERROR_NONE &&
                (join == OT_ERROR_NONE || join == OT_ERROR_ALREADY);
      }
      if (!ready) {
        otUdpClose(ot, &discovery_socket_);
        if (otUdpIsOpen(ot, &data_socket_)) otUdpClose(ot, &data_socket_);
      }
    }
  }
  esp_openthread_lock_release();
  if (ready) ESP_LOGI(TAG, "UDP discovery/data listeners ready");
  return ready;
}
bool ThreadGateway::send_(const otIp6Address &address, uint16_t port, const std::string &payload, bool discovery) {
  if (!transport_ready_ || payload.empty() || payload.size() > 1232 || !esp_openthread_lock_acquire(0)) return false;
  auto *ot = esp_openthread_get_instance();
  bool ok = false;
  if (ot && otThreadGetDeviceRole(ot) >= OT_DEVICE_ROLE_CHILD) {
    otMessageInfo info{};
    info.mPeerAddr = address; info.mPeerPort = port;
    info.mSockAddr = *otThreadGetMeshLocalEid(ot);
    info.mHopLimit = 64;
    auto *msg = otUdpNewMessage(ot, nullptr);
    if (msg) {
      const auto append = otMessageAppend(msg, payload.data(), payload.size());
      const auto result = append == OT_ERROR_NONE ?
          otUdpSend(ot, discovery ? &discovery_socket_ : &data_socket_, msg, &info) : append;
      ok = result == OT_ERROR_NONE;
      if (!ok) otMessageFree(msg);
    }
  }
  esp_openthread_lock_release();
  return ok;
}
void ThreadGateway::offer_(const otIp6Address &address, uint16_t port, const cJSON *request_id) {
  if (address_.empty()) return;
  auto json = owned(cJSON_CreateObject());
  cJSON_AddNumberToObject(json.get(), "v", 1);
  cJSON_AddStringToObject(json.get(), "type", "offer");
  cJSON_AddStringToObject(json.get(), "gateway_id", gateway_id_.c_str());
  cJSON_AddStringToObject(json.get(), "address", address_.c_str());
  cJSON_AddNumberToObject(json.get(), "port", data_port_);
  cJSON_AddNumberToObject(json.get(), "ttl_s", address_ttl_);
  cJSON_AddNumberToObject(json.get(), "control_lease_s", CONTROL_LEASE_MS / 1000);
  if (request_id) cJSON_AddItemToObject(json.get(), "request_id", cJSON_Duplicate(request_id, true));
  send_(address, port, encode(json.get()), true);
}

bool ThreadGateway::acknowledge_report_(Node &node, const Packet &packet, const cJSON *json, bool &allow_older) {
  allow_older = false;
  const auto *need_ack = item(json, "need_ack");
  if (!need_ack) return true;  // Legacy/fire-and-forget state packet.
  if (!cJSON_IsBool(need_ack)) { node.errors++; node.last_error = "need_ack must be bool"; return false; }
  if (!cJSON_IsTrue(need_ack)) return true;
  const auto *id = item(json, "report_id"), *part = item(json, "part"), *parts = item(json, "parts");
  if (!integer(id, 1, UINT32_MAX) || !integer(part, 0, 15) || !integer(parts, 1, 16) ||
      part->valuedouble >= parts->valuedouble) {
    node.errors++; node.last_error = "Invalid report receipt header"; return false;
  }
  auto &receipts = node.report_receipts;
  receipts.erase(std::remove_if(receipts.begin(), receipts.end(), [](const ReportReceipt &r) {
    return uint32_t(millis() - r.started) >= REPORT_RECEIPT_RETENTION_MS;
  }), receipts.end());
  const uint32_t report_id = id->valuedouble;
  const uint32_t base_seq = uint32_t(item(json, "seq")->valuedouble) - uint32_t(part->valuedouble);
  auto it = std::find_if(receipts.begin(), receipts.end(), [report_id](const ReportReceipt &r) {
    return r.id == report_id;
  });
  if (it == receipts.end()) {
    if (receipts.size() >= 8) receipts.erase(receipts.begin());
    receipts.push_back({report_id, millis(), base_seq, uint16_t(parts->valuedouble), 0});
    it = receipts.end() - 1;
  }
  if (it->parts != parts->valuedouble || it->base_seq != base_seq) {
    node.errors++; node.last_error = "Inconsistent report part count/sequence"; return false;
  }
  const uint16_t bit = uint16_t(1U << unsigned(part->valuedouble));
  const bool fresh = (it->received & bit) == 0;
  const uint32_t last_seq = it->base_seq + it->parts - 1;
  // Recover a missing earlier page of this round, without applying duplicate pages or
  // rolling state back after a newer round/legacy state packet has already arrived.
  allow_older = fresh && (!node.has_sequence || static_cast<int32_t>(last_seq - node.sequence) >= 0);
  it->received |= bit;
  const uint16_t complete_mask = uint16_t((1UL << it->parts) - 1);
  if (it->received != complete_mask) return true;
  // Receipt ACK only: send immediately, before sequence filtering, value handling or HA publication.
  auto reply = owned(cJSON_CreateObject());
  cJSON_AddNumberToObject(reply.get(), "v", 1);
  cJSON_AddStringToObject(reply.get(), "type", "report_ack");
  cJSON_AddStringToObject(reply.get(), "gateway_id", gateway_id_.c_str());
  cJSON_AddStringToObject(reply.get(), "node", node.id.c_str());
  cJSON_AddStringToObject(reply.get(), "session", node.session.c_str());
  cJSON_AddNumberToObject(reply.get(), "report_id", report_id);
  cJSON_AddNumberToObject(reply.get(), "parts", it->parts);
  cJSON_AddBoolToObject(reply.get(), "complete", true);
  send_(packet.address, packet.port, encode(reply.get()));
  return true;
}

void ThreadGateway::process_(const Packet &packet) {
  received_++;
  const char *end = nullptr;
  auto json = owned(cJSON_ParseWithLengthOpts(packet.data, packet.size + 1, &end, true));
  if (!cJSON_IsObject(json.get()) || number(json.get(), "v") != 1) { errors_++; return; }
  const std::string type = str(json.get(), "type");
  if (packet.discovery) {
    if (type == "discover") {
      const auto *request = item(json.get(), "request_id");
      if ((cJSON_IsString(request) && request->valuestring && std::strlen(request->valuestring) <= 32) ||
          integer(request, 0, UINT32_MAX)) offer_(packet.address, packet.port, request);
      else errors_++;
    } else if (type != "offer") errors_++;
    return;
  }
  if (type != "manifest" && type != "state" && type != "presence" && type != "command_result") { errors_++; return; }
  const std::string id = str(json.get(), "node"), session = str(json.get(), "session");
  if (!identifier(id) || !identifier(session) || !bounded_string(json.get(), "name", 96)) { errors_++; return; }
  const auto *declared_type=item(json.get(),"node_type");
  if(!cJSON_IsString(declared_type) || (str(json.get(),"node_type")!="sed" && str(json.get(),"node_type")!="reed")) {errors_++;return;}
  if(const auto *heartbeat=item(json.get(),"heartbeat_interval_ms");heartbeat && !integer(heartbeat,1000,15000)) {errors_++;return;}
  auto *node = find_node_(id);
  if (!node) {
    if (nodes_.size() >= MAX_NODES || type == "command_result") { errors_++; return; }
    nodes_.emplace_back(); node = &nodes_.back(); node->id = id; node->name = str(json.get(), "name", id);
  }
  if (std::find(node->retired_sessions.begin(), node->retired_sessions.end(), session) != node->retired_sessions.end()) {
    node->duplicates++; return;
  }
  if (type == "command_result" && node->session != session) { node->errors++; return; }
  if (node->session != session) {
    if (!node->session.empty()) {
      node->retired_sessions.push_back(node->session);
      if (node->retired_sessions.size() > 4) node->retired_sessions.erase(node->retired_sessions.begin());
    }
    for (auto &command : commands_) if (command.node == id &&
        (command.status == "queued" || command.status == "waiting")) {
      command.status = "unconfirmed"; command.error = "Node restarted; command was not replayed";
    }
    node->session = session; node->has_sequence = false; node->mode = "unknown"; node->manifest = Manifest{};
    node->report_receipts.clear();
    for(auto &e:node->entities)e.has_source_sequence=false;
    for (auto &live : live_) if (live->node == id) live->notify_on_sync = true;
  }
  if (type == "state") {
    const auto *sequence = item(json.get(), "seq");
    if (!integer(sequence, 0, UINT32_MAX) || !cJSON_IsObject(item(json.get(), "values"))) {
      node->errors++; node->last_error = "Invalid state sequence/values"; return;
    }
    // The total ACK tracks reception only, not validity/application of individual readings.
    if (const auto *ready = item(json.get(), "control_ready"); ready && !cJSON_IsBool(ready)) {
      node->errors++; node->last_error = "control_ready must be bool"; return;
    }
    bool allow_older = false;
    if (!acknowledge_report_(*node, packet, json.get(), allow_older)) return;
    const uint32_t next = static_cast<uint32_t>(sequence->valuedouble);
    if (node->has_sequence && static_cast<int32_t>(next - node->sequence) <= 0 && !allow_older) {
      node->duplicates++; return;
    }
    if (!node->has_sequence || static_cast<int32_t>(next - node->sequence) > 0) node->sequence = next;
    node->has_sequence = true;
  }
  char address[OT_IP6_ADDRESS_STRING_SIZE];
  otIp6AddressToString(&packet.address, address, sizeof(address));
  node->address = address; node->port = packet.port; node->last_seen = millis(); node->seen = true;
  const std::string node_type=str(json.get(),"node_type");
  if(node_type!=node->node_type) {node->node_type=node_type;dirty_=true;}
  if (node->node_type == "reed") {
    const auto heartbeat = number(json.get(), "heartbeat_interval_ms", node->heartbeat_ms);
    if (heartbeat != node->heartbeat_ms) { node->heartbeat_ms = heartbeat; dirty_ = true; }
  }
  node->received++; node->last_packet.assign(packet.data, std::min(packet.size, size_t(2048)));
  // Any new accepted report cancels an older sleeping declaration.
  if (node->mode == "sleeping") node->mode = "awake";
  if (const auto *ready = item(json.get(), "control_ready")) {
    if (!cJSON_IsBool(ready)) { node->errors++; node->last_error = "control_ready must be bool"; return; }
    node->mode = cJSON_IsTrue(ready) ? "online" : "awake";
    if (cJSON_IsTrue(ready)) node->control_seen = millis();
  }
  if (type == "presence" && flag(json.get(), "sleeping")) node->mode = "sleeping";
  if (const auto *debug = item(json.get(), "debug"); cJSON_IsObject(debug)) {
    const std::string value = encode(debug);
    if (value.size() <= 1024) node->debug = value;
  }
  // Unknown state/presence after an explicit deletion must be able to rebuild
  // a live node's manifest without waiting for that physical node to reboot.
  if ((type == "state" || type == "presence") && node->revision == 0 &&
      (!node->manifest_requested || uint32_t(millis() - node->manifest_request_at) >= 1000)) {
    node->manifest_requested = true; node->manifest_request_at = millis();
    auto request = owned(cJSON_CreateObject());
    cJSON_AddNumberToObject(request.get(), "v", 1);
    cJSON_AddStringToObject(request.get(), "type", "manifest_request");
    cJSON_AddStringToObject(request.get(), "gateway_id", gateway_id_.c_str());
    cJSON_AddStringToObject(request.get(), "node", id.c_str());
    cJSON_AddStringToObject(request.get(), "session", session.c_str());
    send_(packet.address, packet.port, encode(request.get()));
  }
  if(type=="presence" && flag(json.get(),"check_manifest")) {
    auto reply=owned(cJSON_CreateObject());
    const bool matched=node->revision!=0 && node->revision==number(json.get(),"revision");
    cJSON_AddNumberToObject(reply.get(),"v",1);
    cJSON_AddStringToObject(reply.get(),"type",matched?"manifest_ack":"manifest_request");
    cJSON_AddStringToObject(reply.get(),"node",id.c_str()); cJSON_AddStringToObject(reply.get(),"session",session.c_str());
    cJSON_AddStringToObject(reply.get(),"gateway_id",gateway_id_.c_str());
    cJSON_AddNumberToObject(reply.get(),"revision",number(json.get(),"revision"));
    cJSON_AddBoolToObject(reply.get(),"accepted",matched); cJSON_AddBoolToObject(reply.get(),"complete",matched);
    const bool missing=std::any_of(node->entities.begin(),node->entities.end(),[](const Entity &e){return e.kind!=Kind::BUTTON && !e.has_source_sequence;});
    cJSON_AddBoolToObject(reply.get(),"request_state",matched && missing);
    send_(packet.address,packet.port,encode(reply.get()));
  }
  if (type == "manifest") {
    const bool accepted = apply_manifest_(*node, json.get());
    if (!accepted) node->errors++;
    auto reply = owned(cJSON_CreateObject());
    cJSON_AddNumberToObject(reply.get(), "v", 1);
    cJSON_AddStringToObject(reply.get(), "type", "manifest_ack");
    cJSON_AddStringToObject(reply.get(), "gateway_id", gateway_id_.c_str());
    cJSON_AddStringToObject(reply.get(), "node", id.c_str());
    cJSON_AddStringToObject(reply.get(), "session", session.c_str());
    cJSON_AddNumberToObject(reply.get(), "revision", number(json.get(), "revision"));
    cJSON_AddBoolToObject(reply.get(), "accepted", accepted);
    cJSON_AddBoolToObject(reply.get(), "complete", accepted && !node->manifest.parts &&
        node->revision == number(json.get(), "revision"));
    if (!accepted) cJSON_AddStringToObject(reply.get(), "error", node->last_error.c_str());
    send_(packet.address, packet.port, encode(reply.get()));
  } else if (type == "state") {
    const cJSON *value;
    cJSON_ArrayForEach(value, item(json.get(), "values")) {
      auto *entity = value->string ? find_entity_(*node, value->string) : nullptr;
      if (entity) {
        const uint32_t seq=number(json.get(),"seq");
        if(!entity->has_source_sequence || static_cast<int32_t>(seq-entity->source_sequence)>0) {
          if(check_value_(*entity,value)) {entity->source_sequence=seq;entity->has_source_sequence=true;}
          update_value_(*node,*entity,value,true);
        }
      }
      else { node->errors++; node->last_error = "State refers to unknown entity; resend full manifest"; }
    }
  } else if (type == "command_result") {
    const auto *result_id = item(json.get(), "command_id"), *success = item(json.get(), "success");
    if (!integer(result_id, 0, UINT32_MAX) || !cJSON_IsBool(success) ||
        !bounded_string(json.get(), "error", 160)) { node->errors++; return; }
    for (auto &command : commands_) {
      if (command.node != id || command.session != session || command.id != static_cast<uint32_t>(result_id->valuedouble)) continue;
      if(command.status!="queued" && command.status!="waiting")continue;
      command.status = cJSON_IsTrue(success) ? "success" : "failed";
      command.error = str(json.get(), "error");
      if (const auto *value = item(json.get(), "value"))
        if (auto *entity = find_entity_(*node, command.entity)) {
          const auto *seq=item(json.get(),"state_seq");
          if(!seq || (integer(seq,0,UINT32_MAX) && (!entity->has_source_sequence || static_cast<int32_t>(uint32_t(seq->valuedouble)-entity->source_sequence)>0))) {
            if(seq && check_value_(*entity,value)){entity->source_sequence=seq->valuedouble;entity->has_source_sequence=true;}
            update_value_(*node,*entity,value);
          }
        }
      break;
    }
  }
}
void ThreadGateway::send_control(const std::string &node,const std::string &entity,const cJSON *value) {
  auto request=owned(cJSON_CreateObject());cJSON_AddStringToObject(request.get(),"entity",entity.c_str());
  cJSON_AddStringToObject(request.get(),"operation",value?"set":"press");
  if(value)cJSON_AddItemToObject(request.get(),"value",cJSON_Duplicate(value,true));
  int status=200;auto response=owned(web_post(("/thread_gateway/api/node/"+node+"/command").c_str(),request.get(),status));
  if(status>=400) ESP_LOGW(TAG,"Control %s/%s rejected: %s",node.c_str(),entity.c_str(),str(response.get(),"error").c_str());
}
void ThreadGateway::service_commands_() {
  std::set<std::string> occupied;
  for (auto &command : commands_) {
    if (command.status != "queued" && command.status != "waiting") continue;
    if(!occupied.insert(command.node).second)continue;
    auto *node = find_node_(command.node);
    if (!node || !controllable_(*node) || node->session != command.session) {
      command.status = "unconfirmed"; command.error = "Node is not control-ready; command was not queued offline"; continue;
    }
    if (command.attempts && static_cast<uint32_t>(millis() - command.last_attempt) < 1000) continue;
    if (command.attempts >= 3) { command.status = "unconfirmed"; command.error = "No execution result after 3 attempts"; continue; }
    otIp6Address address{};
    command.attempts++; command.last_attempt = millis(); command.status = "waiting";
    if (otIp6AddressFromString(node->address.c_str(), &address) != OT_ERROR_NONE ||
        !send_(address, node->port, command.payload)) command.error = "Local UDP submission failed";
  }
}
void ThreadGateway::loop() {
  if (is_failed() || shutting_down_) return;
  Lock lock(mutex_); if (!lock.acquired) return;
  if (reboot_pending_ && static_cast<int32_t>(millis() - restart_at_) >= 0) {
    reboot_pending_ = false; App.safe_reboot(); return;
  }
  if (!transport_ready_ && (last_init_ == 0 || static_cast<uint32_t>(millis() - last_init_) >= 5000)) {
    last_init_ = millis(); transport_ready_ = start_transport_();
  }
  if (transport_ready_ && esp_openthread_lock_acquire(0)) {
    auto *ot = esp_openthread_get_instance();
    attached_ = ot && otThreadGetDeviceRole(ot) >= OT_DEVICE_ROLE_CHILD;
    std::string current;
    if (attached_) {
      char address[OT_IP6_ADDRESS_STRING_SIZE];
      otIp6AddressToString(otThreadGetMeshLocalEid(ot), address, sizeof(address)); current = address;
    }
    esp_openthread_lock_release();
    if (attached_ && (current != address_ || last_announce_ == 0 ||
        static_cast<uint32_t>(millis() - last_announce_) >= announce_ms_)) {
      address_ = current; last_announce_ = millis();
      otIp6Address group{};
      if (otIp6AddressFromString(group_.c_str(), &group) == OT_ERROR_NONE) offer_(group, discovery_port_, nullptr);
    }
  }
  Packet *packet;
  // Bound work per loop; OpenThread callbacks only enqueue and never publish entities.
  for (unsigned k = 0; k < 4 && xQueueReceive(packets_, &packet, 0) == pdTRUE; ++k) {
    process_(*packet); delete packet;
  }
  for (auto &node : nodes_) if (node.manifest.parts &&
      static_cast<uint32_t>(millis() - node.manifest.started_at) > 15000) {
    node.manifest = Manifest{}; node.last_error = "Incomplete manifest expired; resend all parts"; node.errors++;
  }
  for (auto &node : nodes_) {
    auto &receipts = node.report_receipts;
    receipts.erase(std::remove_if(receipts.begin(), receipts.end(), [](const ReportReceipt &r) {
      return uint32_t(millis() - r.started) >= REPORT_RECEIPT_RETENTION_MS;
    }), receipts.end());
  }
  service_commands_();
  lock.release();
  dispatch_report_events_();
}

cJSON *ThreadGateway::node_json_(const Node &node, bool details) const {
  auto *j = cJSON_CreateObject();
  cJSON_AddStringToObject(j, "id", node.id.c_str());
  cJSON_AddStringToObject(j, "name", node.name.c_str());
  cJSON_AddNumberToObject(j, "ha_device_id", fnv1a_hash("thread_gateway_" + node.id));
  const LiveDevice *active_device = nullptr;
  for (const auto &device : live_devices_) if (device->node == node.id) active_device = device.get();
  cJSON_AddBoolToObject(j, "ha_device_active", active_device != nullptr);
  cJSON_AddStringToObject(j, "ha_device_name", active_device ? active_device->name.c_str() : node.name.c_str());
  cJSON_AddStringToObject(j, "session", node.session.c_str());
  cJSON_AddStringToObject(j, "address", node.address.c_str());
  cJSON_AddNumberToObject(j, "port", node.port);
  const char *mode=offline_(node)?"offline":
      (node.node_type=="reed" && node.seen)?"online":
      (node.mode=="online"&&!controllable_(node))?"awake":node.mode.c_str();
  cJSON_AddStringToObject(j,"mode",mode);
  cJSON_AddStringToObject(j,"node_type",node.node_type.c_str());
  cJSON_AddBoolToObject(j,"offline",offline_(node));
  if(node.node_type=="reed") {
    cJSON_AddNumberToObject(j,"heartbeat_interval_ms",node.heartbeat_ms);
    cJSON_AddNumberToObject(j,"offline_after_ms",node.heartbeat_ms*reed_heartbeat_misses_);
  }
  cJSON_AddBoolToObject(j, "control_ready", controllable_(node));
  if (node.seen) cJSON_AddNumberToObject(j, "last_seen_ms", uint32_t(millis() - node.last_seen));
  else cJSON_AddNullToObject(j, "last_seen_ms");
  cJSON_AddNumberToObject(j, "revision", node.revision);
  cJSON_AddNumberToObject(j, "received", node.received);
  cJSON_AddNumberToObject(j, "duplicates", node.duplicates);
  cJSON_AddNumberToObject(j, "errors", node.errors);
  cJSON_AddNumberToObject(j, "entity_count", node.entities.size());
  const auto active_count = std::count_if(live_.begin(), live_.end(),
      [&node](const auto &entity) { return entity->node == node.id; });
  cJSON_AddNumberToObject(j, "active_entities", active_count);
  cJSON_AddStringToObject(j, "last_error", node.last_error.c_str());
  if (!details) return j;
  cJSON_AddNumberToObject(j, "protocol", 1);
  cJSON_AddStringToObject(j, "last_packet", node.last_packet.c_str());
  if (!node.debug.empty()) cJSON_AddItemToObject(j, "debug", cJSON_Parse(node.debug.c_str()));
  auto *entities = cJSON_AddArrayToObject(j, "entities");
  static const char *types[] = {"numeric", "binary", "text", "switch", "number", "select", "button", "input_text", "light", "fan", "cover", "climate", "lock", "valve"};
  for (const auto &e : node.entities) {
    auto *v = cJSON_CreateObject(); cJSON_AddItemToArray(entities, v);
    cJSON_AddStringToObject(v, "id", e.id.c_str()); cJSON_AddStringToObject(v, "name", e.name.c_str());
    cJSON_AddStringToObject(v, "type", types[static_cast<unsigned>(e.kind)]);
    cJSON_AddStringToObject(v, "unit", e.unit.c_str());
    cJSON_AddStringToObject(v, "device_class", e.device_class.c_str());
    cJSON_AddStringToObject(v, "entity_category", e.entity_category.c_str());
    cJSON_AddStringToObject(v, "state_class", e.state_class.c_str());
    cJSON_AddNumberToObject(v, "accuracy_decimals", e.decimals);
    cJSON_AddBoolToObject(v, "disabled_by_default", e.disabled_by_default);
    cJSON_AddBoolToObject(v, "force_update", e.force_update);
    cJSON_AddBoolToObject(v, "trigger_on_initial_state", e.trigger_on_initial_state);
    cJSON_AddStringToObject(v, "icon", e.icon.c_str());
    cJSON_AddBoolToObject(v, "readonly", static_cast<unsigned>(e.kind) <= 2);
    cJSON_AddBoolToObject(v, "exposable", static_cast<unsigned>(e.kind) <= 2 || (node.node_type=="reed"));
    if(!e.capabilities.empty()) cJSON_AddItemToObject(v,"capabilities",cJSON_Parse(e.capabilities.c_str()));
    cJSON_AddBoolToObject(v, "approved", e.approved);
    bool active = false;
    for (const auto &live : live_) if (live->node == node.id && live->id == e.id && live->kind == e.kind) active = true;
    cJSON_AddBoolToObject(v, "active", active);
    cJSON_AddBoolToObject(v, "supported_metadata", metadata_supported_(e));
    if (e.has_value) {
      cJSON_AddItemToObject(v, "value", cJSON_Parse(e.value.c_str()));
      cJSON_AddNumberToObject(v, "value_age_ms", uint32_t(millis() - e.value_at));
    } else cJSON_AddNullToObject(v, "value");
    cJSON_AddNumberToObject(v, "min", e.minimum); cJSON_AddNumberToObject(v, "max", e.maximum);
    cJSON_AddNumberToObject(v, "step", e.step); cJSON_AddNumberToObject(v, "max_length", e.max_length);
    auto *options = cJSON_AddArrayToObject(v, "options");
    for (const auto &option : e.options) cJSON_AddItemToArray(options, cJSON_CreateString(option.c_str()));
  }
  auto *commands = cJSON_AddArrayToObject(j, "commands");
  for (const auto &command : commands_) if (command.node == node.id) {
    auto *v = cJSON_CreateObject(); cJSON_AddItemToArray(commands, v);
    cJSON_AddNumberToObject(v, "id", command.id);
    cJSON_AddStringToObject(v, "entity", command.entity.c_str());
    cJSON_AddStringToObject(v, "status", command.status.c_str());
    cJSON_AddStringToObject(v, "error", command.error.c_str());
    cJSON_AddNumberToObject(v, "attempts", command.attempts);
  }
  return j;
}
cJSON *ThreadGateway::web_get_bridge_(void *ctx, const char *path, int *status) {
  return static_cast<ThreadGateway *>(ctx)->web_get(path, *status);
}
cJSON *ThreadGateway::web_post_bridge_(void *ctx, const char *path, const cJSON *json, int *status) {
  return static_cast<ThreadGateway *>(ctx)->web_post(path, json, *status);
}
cJSON *ThreadGateway::web_get(const char *raw, int &status) {
  Lock lock(mutex_);
  if (!lock.acquired) return failure(status, 503, "Thread gateway busy");
  status = 200;
  const std::string path(raw);
  if (path == "/thread_gateway/api/info" || path == "/thread_gateway/api/nodes") {
    auto *j = cJSON_CreateObject();
    cJSON_AddBoolToObject(j, "enabled", true);
    cJSON_AddStringToObject(j, "gateway_id", gateway_id_.c_str());
    cJSON_AddStringToObject(j, "address", address_.c_str());
    cJSON_AddBoolToObject(j, "attached", attached_);
    cJSON_AddBoolToObject(j, "pending_restart", dirty_);
    cJSON_AddBoolToObject(j, "reboot_pending", reboot_pending_);
    cJSON_AddBoolToObject(j, "entity_auto_register", auto_register_);
    cJSON_AddStringToObject(j, "registry_error", registry_error_.c_str());
    cJSON_AddNumberToObject(j, "port", data_port_); cJSON_AddNumberToObject(j, "ttl_s", address_ttl_);
    cJSON_AddNumberToObject(j, "received", received_); cJSON_AddNumberToObject(j, "errors", errors_);
    cJSON_AddNumberToObject(j, "dropped", dropped_.load());
    cJSON_AddNumberToObject(j, "active_entities", live_.size());
    cJSON_AddNumberToObject(j, "active_devices", live_devices_.size());
    cJSON_AddNumberToObject(j,"reed_heartbeat_misses",reed_heartbeat_misses_);
    auto *capacity = cJSON_AddObjectToObject(j, "capacity");
    auto *approved = cJSON_AddObjectToObject(j, "approved");
    cJSON_AddNumberToObject(capacity, "nvs", registry_limit_);
    auto *storage = cJSON_AddObjectToObject(j, "nvs");
    cJSON_AddNumberToObject(storage, "registry_bytes", registry_bytes_);
    cJSON_AddNumberToObject(storage, "saved_bytes", registry_saved_bytes_);
    cJSON_AddNumberToObject(storage, "limit_bytes", registry_limit_);
    cJSON_AddBoolToObject(storage, "over_limit", registry_bytes_ > registry_limit_);
    const auto *partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_NVS, "nvs");
    if (partition) cJSON_AddNumberToObject(storage, "partition_bytes", partition->size);
    nvs_stats_t stats{};
    const esp_err_t stats_error = nvs_get_stats("nvs", &stats);
    cJSON_AddBoolToObject(storage, "stats_available", stats_error == ESP_OK);
    if (stats_error == ESP_OK) {
      cJSON_AddNumberToObject(storage, "used_entry_bytes", stats.used_entries * 32);
      cJSON_AddNumberToObject(storage, "free_entry_bytes", stats.free_entries * 32);
    } else cJSON_AddStringToObject(storage, "stats_error", esp_err_to_name(stats_error));
    unsigned counts[14]{};
    for (const auto &node : nodes_) for (const auto &e : node.entities)
      if (e.approved) counts[static_cast<unsigned>(e.kind)]++;
    const char *keys[] = {"sensors", "binary_sensors", "text_sensors", "switches", "numbers", "selects", "buttons", "texts", "lights", "fans", "covers", "climates", "locks", "valves"};
    for (unsigned k = 0; k < capacity_.size(); k++) {
      cJSON_AddNumberToObject(capacity, keys[k], capacity_[k]);
      cJSON_AddNumberToObject(approved, keys[k], counts[k]);
    }
    if (path == "/thread_gateway/api/nodes") {
      auto *nodes = cJSON_AddArrayToObject(j, "nodes");
      for (const auto &node : nodes_) cJSON_AddItemToArray(nodes, node_json_(node, false));
    }
    return j;
  }
  const std::string prefix = "/thread_gateway/api/node/";
  if (path.compare(0, prefix.size(), prefix) == 0) {
    const std::string id = path.substr(prefix.size());
    if (identifier(id)) if (auto *node = find_node_(id)) return node_json_(*node, true);
  }
  return failure(status, 404, "Node or endpoint not found");
}
cJSON *ThreadGateway::web_post(const char *raw, const cJSON *request, int &status) {
  Lock lock(mutex_);
  if (!lock.acquired) return failure(status, 503, "Thread gateway busy");
  status = 200;
  if (shutting_down_ || reboot_pending_) return failure(status, 409, "Gateway is restarting");
  if (!cJSON_IsObject(request)) return failure(status, 400, "Expected JSON object");
  const std::string path(raw);
  if (path == "/thread_gateway/api/apply") {
    if (!flag(request, "confirm")) return failure(status, 400, "Reboot confirmation required");
    std::string error;
    if (!approved_fit_(error)) return failure(status, 409, error);
    if (!save_registry_()) return failure(status, 500, registry_error_);
    reboot_pending_ = true; restart_at_ = millis() + 750;
    auto *j = cJSON_CreateObject(); cJSON_AddBoolToObject(j, "reboot_pending", true); return j;
  }
  const std::string prefix = "/thread_gateway/api/node/";
  if (path.compare(0, prefix.size(), prefix) != 0) return failure(status, 404, "Unknown endpoint");
  const auto slash = path.find('/', prefix.size());
  if (slash == std::string::npos) return failure(status, 404, "Unknown endpoint");
  const std::string id = path.substr(prefix.size(), slash - prefix.size()), action = path.substr(slash);
  auto *node = identifier(id) ? find_node_(id) : nullptr;
  if (!node) return failure(status, 404, "Unknown node");
  if (action == "/delete") {
    if (!flag(request, "confirm")) return failure(status, 400, "Node deletion confirmation required");
    const auto position = static_cast<size_t>(node - nodes_.data());
    const size_t previous_registry_bytes = registry_bytes_;
    Node removed = std::move(nodes_[position]);
    nodes_.erase(nodes_.begin() + position);
    if (!save_registry_()) {
      nodes_.insert(nodes_.begin() + position, std::move(removed));
      registry_bytes_ = previous_registry_bytes;
      return failure(status, 500, registry_error_);
    }
    commands_.erase(std::remove_if(commands_.begin(), commands_.end(),
        [&id](const Command &c) { return c.node == id; }), commands_.end());
    // App owns this boot's API registry. Keep live_ untouched until reboot.
    // Future node reports may rediscover this node; deletion is not a blocklist.
    dirty_ = true;
    auto *j = cJSON_CreateObject();
    cJSON_AddBoolToObject(j, "ok", true);
    cJSON_AddBoolToObject(j, "pending_restart", true);
    return j;
  }
  if (action == "/registration") {
    const auto *entities = item(request, "entities");
    if (!cJSON_IsArray(entities) || cJSON_GetArraySize(entities) > int(MAX_PER_NODE))
      return failure(status, 400, "Expected entity id list");
    std::set<std::string> ids;
    const cJSON *value;
    cJSON_ArrayForEach(value, entities) {
      if (!cJSON_IsString(value) || !value->valuestring || !ids.insert(value->valuestring).second)
        return failure(status, 400, "Invalid or duplicate entity id");
      auto *e = find_entity_(*node, value->valuestring);
      if (!e || (static_cast<unsigned>(e->kind) > 2 && !(node->node_type=="reed"))) return failure(status, 400, "SED controls remain Web-only");
    }
    const size_t previous_registry_bytes = registry_bytes_;
    std::vector<bool> previous;
    bool changed = false;
    for (auto &e : node->entities) {
      previous.push_back(e.approved);
      const bool approved = ids.count(e.id);
      changed |= approved != e.approved; e.approved = approved;
    }
    std::string error;
    bool success = approved_fit_(error);
    if (success) success = save_registry_();
    if (!success) {
      for (size_t k = 0; k < previous.size(); k++) node->entities[k].approved = previous[k];
      registry_bytes_ = previous_registry_bytes;
      return failure(status, 409, error.empty() ? registry_error_ : error);
    }
    if (changed) dirty_ = true;
    auto *j = cJSON_CreateObject(); cJSON_AddBoolToObject(j, "ok", true);
    cJSON_AddBoolToObject(j, "pending_restart", dirty_); return j;
  }
  if (action != "/command") return failure(status, 404, "Unknown endpoint");
  if (!controllable_(*node)) return failure(status, 409, "Node is not in online control-ready mode");
  const std::string entity_id = str(request, "entity");
  auto *entity = find_entity_(*node, entity_id);
  if (!entity || static_cast<unsigned>(entity->kind) <= 2) return failure(status, 400, "Unknown control entity");
  const std::string operation = str(request, "operation");
  if (operation != (entity->kind == Kind::BUTTON ? "press" : "set"))
    return failure(status, 400, "Invalid control operation");
  const auto *value = item(request, "value");
  if (!check_value_(*entity, value, true)) return failure(status, 400, "Control value is invalid or out of range");
  unsigned pending = 0, node_pending = 0;
  for (const auto &command : commands_) if (command.status == "queued" || command.status == "waiting") {
    if (command.node == id) node_pending++;
    pending++;
  }
  if(node_pending >= 8)return failure(status,409,"Node command queue is full");
  if (pending >= 16) return failure(status, 503, "Command capacity exceeded");
  if (commands_.size() >= 32) {
    auto old = std::find_if(commands_.begin(), commands_.end(), [](const Command &c) {
      return c.status != "queued" && c.status != "waiting";
    });
    if (old != commands_.end()) commands_.erase(old);
  }
  Command command; command.node = id; command.entity = entity_id; command.session = node->session;
  command.id = ++command_id_; if(command.id==0)command.id=++command_id_;
  auto j = owned(cJSON_CreateObject());
  cJSON_AddNumberToObject(j.get(), "v", 1); cJSON_AddStringToObject(j.get(), "type", "command");
  cJSON_AddStringToObject(j.get(), "node", id.c_str());
  cJSON_AddStringToObject(j.get(), "session", node->session.c_str());
  cJSON_AddStringToObject(j.get(), "gateway_id", gateway_id_.c_str());
  cJSON_AddNumberToObject(j.get(), "command_id", command.id);
  cJSON_AddStringToObject(j.get(), "entity", entity_id.c_str());
  cJSON_AddStringToObject(j.get(), "operation", operation.c_str());
  if (value) cJSON_AddItemToObject(j.get(), "value", cJSON_Duplicate(value, true));
  command.payload = encode(j.get());
  if (command.payload.size() > 1232) return failure(status, 400, "Command exceeds UDP payload limit");
  const auto command_id = command.id;
  commands_.push_back(std::move(command));
  auto *result = cJSON_CreateObject(); cJSON_AddNumberToObject(result, "command_id", command_id);
  cJSON_AddStringToObject(result, "status", "queued"); return result;
}
}  // namespace esphome::thread_gateway
#endif
