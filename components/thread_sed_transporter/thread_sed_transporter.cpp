#include "thread_sed_transporter.h"
#include <algorithm>
#include <cstring>
#include <new>
#include <cstdio>
#include <cctype>
#include "esphome/core/log.h"
#include "esphome/core/alloc_helpers.h"
#include "esp_attr.h"
#include "esp_system.h"
#include "esp_random.h"
#include "esp_rtc_time.h"
#include "esp_openthread.h"
#include "esp_openthread_lock.h"
#include "openthread/ip6.h"
#include "openthread/message.h"

#ifndef USE_ESP32_RTC_PREFERENCES_STORAGE
#error "thread_sed_transporter requires RTC preferences"
#endif
namespace esphome::thread_sed_transporter {
static const char *const TAG = "thread_sed_transporter";
static constexpr uint32_t SAMPLE_VERSION = 0x53535231;
static constexpr size_t MAX_PENDING_REPORT_ACKS = 8;
namespace {
struct RetainedTransport {
  uint32_t version, identity, rounds, ttl_s;
  uint64_t expires;
  otIp6Address address;
  uint16_t port;
  char gateway[33];
  bool full_due;
  uint32_t checksum;
};
static RTC_DATA_ATTR RetainedTransport retained{};
uint32_t checksum() {
  const auto *p = reinterpret_cast<const uint8_t *>(&retained);
  uint32_t hash = 2166136261U;
  for (size_t k = 0; k < offsetof(RetainedTransport, checksum); k++) hash = (hash ^ p[k]) * 16777619U;
  return hash;
}
void retain() { retained.checksum = checksum(); }
const cJSON *item(const cJSON *j, const char *key) { return cJSON_GetObjectItemCaseSensitive(j, key); }
std::string str(const cJSON *j, const char *key) {
  const auto *v = item(j, key);
  return cJSON_IsString(v) && v->valuestring ? v->valuestring : "";
}
bool uint_value(const cJSON *j, uint32_t min = 0, uint32_t max = UINT32_MAX) {
  return cJSON_IsNumber(j) && std::isfinite(j->valuedouble) && j->valuedouble >= min &&
      j->valuedouble <= max && std::floor(j->valuedouble) == j->valuedouble;
}
std::string encode(const cJSON *j) {
  char *raw = cJSON_PrintUnformatted(j);
  if (!raw) return "";
  std::string result(raw); cJSON_free(raw); return result;
}

}

void ReportEntry::dump_config() {
  ESP_LOGCONFIG(TAG, "  Bound entity '%s' (%s)", id_.c_str(), endpoint_.entity->get_name().c_str());
}

bool ReportSensor::initialize(bool restore) {
  preference_ = global_preferences->make_preference<RetainedSample>(storage_key_, false);
  RetainedSample saved{};
  if (restore && preference_.load(&saved) && saved.version == SAMPLE_VERSION && finite_sample(saved.baseline)) sample_ = saved;
  return preference_.save(&sample_);
}
ReportOutcome ReportSensor::stage() {
  pending_.reset();
  auto *value = endpoint_.read();
  if (cJSON_IsNumber(value)) pending_ = static_cast<float>(value->valuedouble);
  cJSON_Delete(value);
  if (!pending_.has_value() || !finite_sample(*pending_)) return ReportOutcome::INVALID;
  return should_report(*pending_, sample_.baseline, delta_, sample_.version == SAMPLE_VERSION) ?
      ReportOutcome::CHANGED : ReportOutcome::UNCHANGED;
}
cJSON *ReportSensor::value() const { return pending_.has_value() ? cJSON_CreateNumber(*pending_) : nullptr; }
bool ReportSensor::commit() {
  if (!pending_.has_value()) return false;
  RetainedSample next{SAMPLE_VERSION, *pending_};
  if (!preference_.save(&next)) return false;
  sample_ = next;
  return true;
}

bool ReportBinarySensor::initialize(bool restore) {
  preference_ = global_preferences->make_preference<Sample>(storage_key_, false);
  Sample saved{};
  if (restore && preference_.load(&saved) && saved.version == SAMPLE_VERSION && saved.baseline <= 1) sample_ = saved;
  return preference_.save(&sample_);
}
ReportOutcome ReportBinarySensor::stage() {
  pending_.reset();
  auto *value = endpoint_.read();
  if (cJSON_IsBool(value)) pending_ = cJSON_IsTrue(value);
  cJSON_Delete(value);
  if (!pending_.has_value()) return ReportOutcome::INVALID;
  return changed_value(*pending_, bool(sample_.baseline), sample_.version == SAMPLE_VERSION) ?
      ReportOutcome::CHANGED : ReportOutcome::UNCHANGED;
}
cJSON *ReportBinarySensor::value() const { return pending_.has_value() ? cJSON_CreateBool(*pending_) : nullptr; }
bool ReportBinarySensor::commit() {
  if (!pending_.has_value()) return false;
  Sample next{SAMPLE_VERSION, *pending_ ? 1U : 0U};
  if (!preference_.save(&next)) return false;
  sample_ = next;
  return true;
}

bool ReportTextSensor::initialize(bool restore) {
  const unsigned size = TEXT_HEADER_SIZE + max_length_;
  preference_ = global_preferences->make_preference(size, storage_key_, false);
  TextRecord saved{};
  if (restore && preference_.load(saved.data(), size) && valid_text_record(saved, max_length_)) {
    record_ = saved; baseline_.assign(reinterpret_cast<const char *>(saved.data() + TEXT_HEADER_SIZE), get_u32(saved, 4));
    has_baseline_ = true;
  }
  return preference_.save(record_.data(), size);
}
ReportOutcome ReportTextSensor::stage() {
  pending_.reset();
  auto *value = endpoint_.read();
  if (cJSON_IsString(value) && value->valuestring) pending_ = std::string(value->valuestring);
  cJSON_Delete(value);
  if (!pending_.has_value() || pending_->size() > max_length_ || pending_->find('\0') != std::string::npos)
    return ReportOutcome::INVALID;
  return changed_value(*pending_, baseline_, has_baseline_) ? ReportOutcome::CHANGED : ReportOutcome::UNCHANGED;
}
cJSON *ReportTextSensor::value() const { return pending_.has_value() ? cJSON_CreateString(pending_->c_str()) : nullptr; }
bool ReportTextSensor::commit() {
  if (!pending_.has_value()) return false;
  TextRecord next{};
  if (!encode_text(next, *pending_, max_length_) || !preference_.save(next.data(), TEXT_HEADER_SIZE + max_length_)) return false;
  record_ = next; baseline_ = *pending_; has_baseline_ = true;
  return true;
}

void ThreadSedTransporter::setup() {
  bool restore = esp_reset_reason() == ESP_RST_DEEPSLEEP && retained.version == 2 &&
      retained.identity == storage_identity_ && retained.checksum == checksum() &&
      retained.gateway[32] == 0;
  if (!restore) {
    retained = {}; retained.version = 2; retained.identity = storage_identity_; retained.full_due = true; retain();
  }
  for (auto *entry : sensors_) {
    if (!entry->initialize(restore)) { ESP_LOGE(TAG, "RTC baseline allocation failed"); mark_failed(); return; }
  }
  packets_ = xQueueCreate(6, sizeof(Packet *));
  if (!packets_) { mark_failed(); return; }
  char session[17]; snprintf(session, sizeof(session), "%08x%08x", unsigned(esp_random()), unsigned(esp_random()));
  session_ = session;
}
void ThreadSedTransporter::add_update_component(PollingComponent *component) {
  if (std::find(update_components_.begin(), update_components_.end(), component) == update_components_.end())
    update_components_.push_back(component);
}
void ThreadSedTransporter::receive_(void *ctx, otMessage *message, const otMessageInfo *info) {
  auto *self = static_cast<ThreadSedTransporter *>(ctx);
  if (self->shutdown_ || !self->packets_) return;
  const int length = otMessageGetLength(message) - otMessageGetOffset(message);
  if (length <= 0 || length > 1232) { self->drops_++; return; }
  auto *p = new (std::nothrow) Packet;
  if (!p) { self->drops_++; return; }
  p->peer = info->mPeerAddr; p->port = info->mPeerPort; p->size = length;
  if (otMessageRead(message, otMessageGetOffset(message), p->data, length) != length) { delete p; self->drops_++; return; }
  p->data[length] = 0;
  if (xQueueSend(self->packets_, &p, 0) != pdTRUE) { delete p; self->drops_++; }
}
bool ThreadSedTransporter::open_() {
  if (opened_) return true;
  if (!ot_ || !ot_->is_lock_initialized() || !ot_->is_connected() || !esp_openthread_lock_acquire(0)) return false;
  auto *instance = esp_openthread_get_instance();
  bool success = false;
  if (instance && otThreadGetDeviceRole(instance) >= OT_DEVICE_ROLE_CHILD &&
      otUdpOpen(instance, &socket_, receive_, this) == OT_ERROR_NONE) {
    otSockAddr local{};
    success = otUdpBind(instance, &socket_, &local, OT_NETIF_THREAD_INTERNAL) == OT_ERROR_NONE;
    if (!success) otUdpClose(instance, &socket_);
  }
  esp_openthread_lock_release();
  return opened_ = success;
}
bool ThreadSedTransporter::send_(const otIp6Address &address, uint16_t port, const std::string &payload) {
  if (shutdown_ || !opened_ || payload.empty() || payload.size() > 1232 ||
      !ot_->is_lock_initialized() || !esp_openthread_lock_acquire(0)) return false;
  auto *instance = esp_openthread_get_instance();
  bool success = false;
  if (instance && otThreadGetDeviceRole(instance) >= OT_DEVICE_ROLE_CHILD) {
    otMessageInfo info{}; info.mPeerAddr = address; info.mPeerPort = port;
    info.mSockAddr = *otThreadGetMeshLocalEid(instance); info.mHopLimit = 64;
    if (auto *message = otUdpNewMessage(instance, nullptr)) {
      const auto append = otMessageAppend(message, payload.data(), payload.size());
      const auto result = append == OT_ERROR_NONE ? otUdpSend(instance, &socket_, message, &info) : append;
      success = result == OT_ERROR_NONE;
      if (!success) otMessageFree(message);
    }
  }
  esp_openthread_lock_release();
  return success;
}
bool ThreadSedTransporter::target_valid_() const {
  return retained.port && retained.gateway[0] && esp_rtc_get_time_us() < retained.expires &&
      (expected_gateway_.empty() || expected_gateway_ == retained.gateway);
}
void ThreadSedTransporter::renew_target_() {
  if (!retained.ttl_s) return;
  retained.expires = esp_rtc_get_time_us() + uint64_t(retained.ttl_s) * 1000000ULL;
  retain();
}
void ThreadSedTransporter::discovery() {
  // Keep staged samples and frozen reports. Discovery never starts acquisition.
  retained.expires = 0; retain();
  confirmed_ = false; querying_ = false;
  query_attempts_ = 0; manifest_attempts_ = 0;
  discovery_active_ = true; discovery_started_ = millis();
  for (auto &ack : pending_acks_) {
    if (ack.result == PendingAck::Result::WAITING && !ack.recovering) {
      ack.recovering = true; ack.recovery_at = discovery_started_;
    }
  }
  ESP_LOGD(TAG, "Gateway rediscovery requested");
  if (!shutdown_ && !is_failed()) ensure_target_();
}
cJSON *ThreadSedTransporter::envelope_(const char *type) const {
  auto *j = cJSON_CreateObject();
  cJSON_AddNumberToObject(j, "v", 1); cJSON_AddStringToObject(j, "type", type);
  cJSON_AddStringToObject(j, "node_type", "sed");
  cJSON_AddStringToObject(j, "node", node_.c_str()); cJSON_AddStringToObject(j, "session", session_.c_str());
  return j;
}
bool ThreadSedTransporter::ensure_target_() {
  if (target_valid_()) return true;
  if (confirmed_) {
    ESP_LOGD(TAG, "Gateway target expired; rediscovering before the next report");
    query_attempts_ = 0;
    manifest_attempts_ = 0;
    querying_ = false;
  }
  confirmed_ = false;
  if (!open_()) return false;
  const uint32_t now = millis();
  if (querying_ && uint32_t(now - query_at_) < discovery_timeout_) return false;
  if (query_attempts_ >= 3) return false;
  query_at_ = now; querying_ = true; request_id_ = esp_random(); query_attempts_++;
  auto *j = cJSON_CreateObject();
  cJSON_AddNumberToObject(j, "v", 1); cJSON_AddStringToObject(j, "type", "discover");
  cJSON_AddNumberToObject(j, "request_id", request_id_);
  otIp6Address group{};
  if (otIp6AddressFromString(group_.c_str(), &group) == OT_ERROR_NONE) send_(group, discovery_port_, encode(j));
  cJSON_Delete(j); return false;
}
bool ThreadSedTransporter::prepare_manifest_() {
  std::vector<std::string> descriptors;
  for (const auto *e : sensors_) {
    auto *j = e->descriptor();
    if (!j) return false;
    if (str(j, "name").empty()) cJSON_ReplaceItemInObjectCaseSensitive(j, "name", cJSON_CreateString(str(j, "id").c_str()));
    if (str(j, "name").size() > 96 || !thread_entity_bridge::validate_capabilities(str(j, "type"), item(j, "capabilities"))) {
      cJSON_Delete(j); return false;
    }
    descriptors.push_back(encode(j)); cJSON_Delete(j);
  }
  for (const auto &c : controls_) {
    auto *j = c.descriptor();
    if (!j) return false;
    if (str(j, "name").empty()) cJSON_ReplaceItemInObjectCaseSensitive(j, "name", cJSON_CreateString(str(j, "id").c_str()));
    if (str(j, "name").size() > 96 || !thread_entity_bridge::validate_capabilities(str(j, "type"), item(j, "capabilities"))) {
      cJSON_Delete(j); return false;
    }
    descriptors.push_back(encode(j)); cJSON_Delete(j);
  }
  auto pack = [this](const std::vector<std::string> &page, unsigned index, unsigned parts) {
    auto *j = envelope_("manifest");
    cJSON_AddStringToObject(j, "name", name_.c_str()); cJSON_AddNumberToObject(j, "revision", revision_);
    cJSON_AddNumberToObject(j, "part", index); cJSON_AddNumberToObject(j, "parts", parts);
    auto *a = cJSON_AddArrayToObject(j, "entities");
    for (const auto &d : page) cJSON_AddItemToArray(a, cJSON_Parse(d.c_str()));
    std::string payload = encode(j); cJSON_Delete(j); return payload;
  };
  std::vector<std::vector<std::string>> pages(1);
  for (const auto &d : descriptors) {
    pages.back().push_back(d);
    if (pack(pages.back(),15,16).size() > 1232) {
      pages.back().pop_back();
      if (pages.back().empty() || pages.size() >= 16) return false;
      pages.push_back({d});
      if (pack(pages.back(),15,16).size() > 1232) return false;
    }
  }
  manifest_.clear();
  for (unsigned k = 0; k < pages.size(); k++) manifest_.push_back(pack(pages[k], k, pages.size()));
  return true;
}
void ThreadSedTransporter::service_manifest_() {
  // Revalidate TTL even after the manifest was confirmed. Otherwise an expired
  // target leaves confirmed_ set forever, blocking both discovery and reports.
  if (!ensure_target_() || confirmed_ || !opened_) return;
  if (manifest_.empty() && !prepare_manifest_()) {
    ESP_LOGE(TAG, "Manifest cannot fit UDP limits or control traits are invalid"); mark_failed(); return;
  }
  if (manifest_attempts_ && uint32_t(millis() - manifest_at_) < 400) return;
  if (manifest_attempts_ >= 3) return;
  manifest_at_ = millis(); manifest_attempts_++;
  for (const auto &payload : manifest_) if (!send_(retained.address, retained.port, payload)) break;
}
void ThreadSedTransporter::process_(const Packet &p) {
  auto *j = cJSON_ParseWithLengthOpts(p.data, p.size + 1, nullptr, true);
  if (!cJSON_IsObject(j) || !uint_value(item(j, "v"), 1, 1)) { cJSON_Delete(j); return; }
  const std::string type = str(j, "type");
  if (type == "offer") {
    const std::string gateway = str(j, "gateway_id"), address = str(j, "address");
    const auto *request = item(j, "request_id");
    otIp6Address target{};
    bool valid_id = !gateway.empty() && gateway.size() <= 32;
    for (unsigned char c : gateway) valid_id &= std::isalnum(c) || c == '_' || c == '-';
    if (querying_ && p.port == discovery_port_ && uint_value(request) &&
        uint32_t(request->valuedouble) == request_id_ && valid_id &&
        (expected_gateway_.empty() || gateway == expected_gateway_) &&
        uint_value(item(j, "port"), 1, 65535) && uint_value(item(j, "ttl_s"), 1, 86400) &&
        otIp6AddressFromString(address.c_str(), &target) == OT_ERROR_NONE &&
        std::memcmp(&target, &p.peer, sizeof(target)) == 0) {
      if (gateway != retained.gateway) commands_.clear();
      retained.address = target; retained.port = item(j, "port")->valuedouble;
      std::memset(retained.gateway, 0, sizeof(retained.gateway));
      std::memcpy(retained.gateway, gateway.data(), gateway.size());
      retained.ttl_s = uint32_t(item(j, "ttl_s")->valuedouble);
      renew_target_();
      retained.full_due = true; retain();
      querying_ = false; confirmed_ = false; manifest_attempts_ = 0;
      ESP_LOGD(TAG, "Discovered gateway '%s', target TTL %us", gateway.c_str(), unsigned(item(j, "ttl_s")->valuedouble));
    }
    cJSON_Delete(j); return;
  }
  if (type == "report_ack") {
    receive_report_ack_(p, j);
    cJSON_Delete(j); return;
  }
  if (!target_valid_() || p.port != retained.port ||
      std::memcmp(&p.peer, &retained.address, sizeof(p.peer)) != 0 ||
      str(j, "node") != node_ || str(j, "session") != session_ || str(j, "gateway_id") != retained.gateway) {
    cJSON_Delete(j); return;
  }
  if (type == "manifest_request") {
    // Already validated the current gateway endpoint, node ID and boot session.
    confirmed_ = false; manifest_attempts_ = 0;
    retained.full_due = true; retain();
    ESP_LOGD(TAG, "Gateway requested a fresh manifest");
  } else if (type == "manifest_ack" && uint_value(item(j, "revision"), revision_, revision_)) {
    if (cJSON_IsTrue(item(j, "accepted")) && cJSON_IsTrue(item(j, "complete"))) {
      if (!confirmed_) presence_dirty_ = true;
      confirmed_ = true;
      last_gateway_reply_ = millis();
      renew_target_();
    } else if (cJSON_IsFalse(item(j, "accepted"))) {
      ESP_LOGW(TAG, "Gateway rejected manifest: %s", str(j, "error").c_str());
      if (reporting_) { summary_.errors++; finish_(); }
      manifest_attempts_ = 3;
    }
  } else if (type == "command") {
    last_gateway_reply_ = millis();
    command_(j);
  }
  cJSON_Delete(j);
}

void ThreadSedTransporter::report(uint32_t ack_timeout, uint8_t ack_resend) {
  if (reporting_) { ESP_LOGW(TAG, "Report already running; ignoring duplicate request"); return; }
  reporting_ = true; summary_ = {}; staged_ = false; started_ = millis(); phase_ = Phase::DELAY;
  report_ack_timeout_ = ack_timeout; report_ack_resend_ = ack_resend;
  need_ack_ = ack_timeout != 0; ack_tracked_ = false; ack_immediate_failure_ = false;
  if (++next_report_id_ == 0) ++next_report_id_;
  report_id_ = next_report_id_;
  ESP_LOGD(TAG, "Starting report %u: ACK=%s wait=%ums resends=%u",
           unsigned(report_id_), YESNO(need_ack_), unsigned(report_ack_timeout_), unsigned(report_ack_resend_));
  query_attempts_ = 0; manifest_attempts_ = 0;
  for (auto *e : sensors_) e->prepare_acquisition();
  if (++retained.rounds >= full_every_) retained.full_due = true;
  retain();
  if (is_failed()) { summary_.errors++; finish_(); }
}
void ThreadSedTransporter::stage_() {
  if (staged_) return;
  staged_ = true;
  for (auto *e : sensors_) {
    e->outcome = e->source_ready() ? e->stage() : ReportOutcome::INVALID;
    summary_.add(e->outcome);
    if (e->outcome == ReportOutcome::INVALID) ESP_LOGW(TAG, "No valid fresh sample for '%s'", e->identity().c_str());
  }
}
void ThreadSedTransporter::transmit_() {
  force_full_ = retained.full_due;
  // Determine the total page count before sending. ACK is independent of commits.
  struct Outgoing {
    std::string payload;
    std::vector<ReportEntry *> commits;
    std::vector<std::pair<size_t, std::string>> controls;
  };
  std::vector<Outgoing> pages;
  auto make_packet = [&]() {
    auto *j = envelope_("state");
    cJSON_AddNumberToObject(j, "seq", UINT32_MAX);
    if (need_ack_) {
      cJSON_AddBoolToObject(j, "need_ack", true);
      cJSON_AddNumberToObject(j, "report_id", report_id_);
      // Reserve the largest page header; each entity fits wholly in one page.
      cJSON_AddNumberToObject(j, "part", 15);
      cJSON_AddNumberToObject(j, "parts", 16);
    }
    cJSON_AddObjectToObject(j, "values");
    return j;
  };
  auto *packet = make_packet();
  auto *values = cJSON_GetObjectItemCaseSensitive(packet, "values");
  std::vector<ReportEntry *> commits;
  std::vector<std::pair<size_t, std::string>> control_commits;
  auto save_page = [&]() {
    if (pages.size() >= 16) { summary_.errors++; return false; }
    auto payload = encode(packet);
    if (payload.empty()) { summary_.errors++; return false; }
    pages.push_back({std::move(payload), commits, control_commits});
    return true;
  };
  auto add = [&](const std::string &id, cJSON *value, ReportEntry *entry, size_t control) {
    if (!value) { summary_.errors++; return false; }
    const std::string wire = encode(value);
    cJSON_AddItemToObject(values, id.c_str(), value);
    if (encode(packet).size() > 1232) {
      value = cJSON_DetachItemFromObjectCaseSensitive(values, id.c_str());
      if (!values->child) { summary_.errors++; cJSON_Delete(value); return false; }
      if (!save_page()) { cJSON_Delete(value); return false; }
      cJSON_Delete(packet); packet = make_packet();
      values = cJSON_GetObjectItemCaseSensitive(packet, "values");
      commits.clear(); control_commits.clear();
      cJSON_AddItemToObject(values, id.c_str(), value);
      if (encode(packet).size() > 1232) { summary_.errors++; return false; }
    }
    if (entry) commits.push_back(entry);
    else control_commits.emplace_back(control, wire);
    return true;
  };
  bool prepared = true;
  for (auto *e : sensors_) {
    if (e->outcome != ReportOutcome::CHANGED && !(force_full_ && e->outcome == ReportOutcome::UNCHANGED)) continue;
    if (!add(e->identity(), e->value(), e, 0)) { prepared = false; break; }
  }
  if (prepared) for (size_t k = 0; k < controls_.size(); k++) {
    auto *value = controls_[k].read();
    if (!value) continue;
    if (!force_full_ && encode(value) == controls_[k].previous) { cJSON_Delete(value); continue; }
    if (!add(controls_[k].id, value, nullptr, k)) { prepared = false; break; }
  }
  // An unchanged report still sends an empty round when a communication ACK was requested.
  if (prepared && (values->child || (need_ack_ && pages.empty()))) prepared = save_page();
  cJSON_Delete(packet);
  // Finalize every sequence number and byte string once. Retries replay these exact bytes.
  std::vector<std::string> payloads;
  bool encoded = true;
  for (size_t k = 0; k < pages.size(); k++) {
    auto *j = cJSON_Parse(pages[k].payload.c_str());
    if (!j) { summary_.errors++; encoded = false; break; }
    cJSON_SetNumberValue(cJSON_GetObjectItemCaseSensitive(j, "seq"), ++sequence_);
    if (need_ack_) {
      cJSON_SetNumberValue(cJSON_GetObjectItemCaseSensitive(j, "part"), k);
      cJSON_SetNumberValue(cJSON_GetObjectItemCaseSensitive(j, "parts"), pages.size());
    }
    auto payload = encode(j); cJSON_Delete(j);
    if (payload.empty()) { summary_.errors++; encoded = false; break; }
    payloads.push_back(std::move(payload));
  }
  ESP_LOGD(TAG, "Report %u: submitting %u state packet(s)", unsigned(report_id_), unsigned(payloads.size()));
  bool submitted = encoded;
  if (encoded) for (size_t k = 0; k < pages.size(); k++) {
    auto &page = pages[k];
    if (!send_(retained.address, retained.port, payloads[k])) { summary_.errors++; submitted = false; break; }
    for (auto *e : page.commits) {
      summary_.sent++;
      if (!e->commit()) summary_.errors++;
    }
    for (const auto &c : page.controls) { controls_[c.first].previous = c.second; summary_.sent++; }
  }
  // Even a locally partial submission may recover by retry; RTC commits above are never replayed.
  if (need_ack_) track_report_ack_(encoded ? std::move(payloads) : std::vector<std::string>{});
  if (prepared && submitted && summary_.success() && force_full_) {
    retained.full_due = false; retained.rounds = 0; retain();
  }
  finish_();
}
void ThreadSedTransporter::track_report_ack_(std::vector<std::string> payloads) {
  ack_tracked_ = true;
  if (payloads.empty()) { ack_immediate_failure_ = true; return; }
  if (pending_acks_.size() >= MAX_PENDING_REPORT_ACKS) {
    ack_immediate_failure_ = true;
    ESP_LOGW(TAG, "Too many pending report ACKs; reporting this communication attempt as unconfirmed");
    return;
  }
  PendingAck ack{};
  ack.id = report_id_; ack.started = ack.attempt_at = millis(); ack.parts = payloads.size();
  ack.timeout = report_ack_timeout_; ack.resend_limit = report_ack_resend_;
  ack.payloads = std::move(payloads);
  ack.peer = retained.address; ack.port = retained.port; ack.gateway = retained.gateway;
  pending_acks_.push_back(std::move(ack));
}
void ThreadSedTransporter::receive_report_ack_(const Packet &packet, const cJSON *json) {
  if (str(json, "node") != node_ || str(json, "session") != session_ ||
      !cJSON_IsTrue(item(json, "complete")) || !uint_value(item(json, "report_id"), 1) ||
      !uint_value(item(json, "parts"), 1, 16)) return;
  for (auto &ack : pending_acks_) {
    if (ack.result != PendingAck::Result::WAITING || ack.id != uint32_t(item(json, "report_id")->valuedouble) ||
        ack.parts != item(json, "parts")->valuedouble || packet.port != ack.port ||
        std::memcmp(&packet.peer, &ack.peer, sizeof(packet.peer)) != 0 || str(json, "gateway_id") != ack.gateway)
      continue;
    // Accept this round's ACK throughout its bounded initial/retry window, once only.
    if (uint32_t(millis() - ack.started) < ack.timeout * (uint32_t(ack.resend_limit) + 1)) {
      ack.result = PendingAck::Result::SUCCESS;
      last_gateway_reply_ = millis();
      if (packet.port == retained.port && str(json, "gateway_id") == retained.gateway &&
          std::memcmp(&packet.peer, &retained.address, sizeof(packet.peer)) == 0) renew_target_();
    }
    return;
  }
}
void ThreadSedTransporter::service_report_ack_() {
  // Erase before callbacks: automations may start a new report and append pending ACKs.
  std::vector<std::pair<bool, uint32_t>> finished;
  for (auto it = pending_acks_.begin(); it != pending_acks_.end();) {
    const uint32_t now = millis();
    const bool success = it->result == PendingAck::Result::SUCCESS;
    // One bounded rediscovery per frozen report; no resampling or RTC commits.
    if (!success && it->recovering) {
      if (uint32_t(now - it->recovery_at) >= timeout_ms_) {
        ESP_LOGW(TAG, "Report %u rediscovery timed out", unsigned(it->id));
        finished.emplace_back(false, it->id);
        it = pending_acks_.erase(it);
        continue;
      }
      if (!confirmed_ || !target_valid_() || !opened_) { ++it; continue; }
      it->peer = retained.address; it->port = retained.port; it->gateway = retained.gateway;
      it->recovering = false; it->resends = 0;
      it->started = it->attempt_at = now;
      ESP_LOGD(TAG, "Report %u resubmitting frozen round after rediscovery", unsigned(it->id));
      for (const auto &payload : it->payloads) send_(it->peer, it->port, payload);
      ++it; continue;
    }
    const bool expired = uint32_t(now - it->started) >= it->timeout * (uint32_t(it->resend_limit) + 1);
    if (!success && expired && !it->recovered) {
      it->recovered = it->recovering = true;
      it->recovery_at = now;
      ESP_LOGW(TAG, "Report %u ACK attempts exhausted; rediscovering once", unsigned(it->id));
      discovery();
      ++it; continue;
    }
    if (!success && !expired) {
      if (uint32_t(now - it->attempt_at) < it->timeout) { ++it; continue; }
      if (it->resends < it->resend_limit) {
        ++it->resends;
        ESP_LOGD(TAG, "Report %u ACK timeout: retransmitting whole round %u/%u",
                 unsigned(it->id), unsigned(it->resends), unsigned(it->resend_limit));
        // No re-acquisition, baseline commit, local publication or summary mutation here.
        for (const auto &payload : it->payloads) send_(it->peer, it->port, payload);
        it->attempt_at = millis();
        ++it; continue;
      }
    }
    if (!success && it->port == retained.port &&
        std::memcmp(&it->peer, &retained.address, sizeof(it->peer)) == 0) {
      retained.expires = 0; retain(); confirmed_ = false;
    }
    finished.emplace_back(success, it->id);
    it = pending_acks_.erase(it);
  }
  for (const auto &result : finished) report_result_(result.first, result.second);
}
void ThreadSedTransporter::report_result_(bool success, uint32_t report_id) {
  ESP_LOGD(TAG, "Report %u communication result: %s", unsigned(report_id), YESNO(success));
  if (success) success_callbacks_.call(report_id);
  else fail_callbacks_.call(report_id);
}
void ThreadSedTransporter::finish_() {
  if (!reporting_) return;
  phase_ = Phase::IDLE;
  if (need_ack_ && !ack_tracked_) track_report_ack_({});
  const bool ack_immediate_failure = ack_immediate_failure_;
  const uint32_t completed_id = report_id_;
  if (!summary_.success()) {
    status_set_warning(); retained.full_due = true;
    if (!confirmed_) { retained.expires = 0; querying_ = false; }
    retain();
  } else status_clear_warning();
  ESP_LOGD(TAG, "Report complete: UDP entities=%u unchanged=%u invalid=%u errors=%u (delivery not confirmed)",
           unsigned(summary_.sent),unsigned(summary_.unchanged),unsigned(summary_.invalid),unsigned(summary_.errors));
  // Keep the guard set throughout synchronous callbacks; recursive report is ignored.
  complete_callbacks_.call(summary_.success(), summary_.sent, summary_.unchanged, summary_.invalid, summary_.errors);
  reporting_ = false;
  if (ack_immediate_failure) report_result_(false, completed_id);
}
void ThreadSedTransporter::presence_(bool ready, bool sleeping) {
  if (!confirmed_ || !target_valid_()) return;
  auto *j = envelope_("presence");
  // Solicit a bounded heartbeat reply without sending another full manifest.
  cJSON_AddBoolToObject(j, "check_manifest", true);
  cJSON_AddNumberToObject(j, "revision", revision_);
  cJSON_AddBoolToObject(j,"control_ready",ready);
  if (sleeping) cJSON_AddBoolToObject(j,"sleeping",true);
  if (send_(retained.address,retained.port,encode(j))) { heartbeat_at_ = millis(); presence_dirty_ = false; }
  cJSON_Delete(j);
}
void ThreadSedTransporter::service_presence_() {
  const bool ready = ready_();
  if (ready != last_ready_) {
    last_ready_ = ready; presence_dirty_ = true;
    if (ready) { query_attempts_ = 0; manifest_attempts_ = 0; }
  }
  if (discovery_active_ && (confirmed_ || uint32_t(millis() - discovery_started_) >= timeout_ms_)) {
    if (!confirmed_) ESP_LOGW(TAG, "Manual/recovery gateway discovery timed out");
    discovery_active_ = false;
  }
  if (!ready && !reporting_ && !discovery_active_) {
    if (presence_dirty_) presence_(false);
    return;
  }
  if (!open_()) return;
  // An online node continues discovering after bounded attempts, without a busy loop.
  if (!reporting_ && query_attempts_ >= 3 && uint32_t(millis() - query_at_) >= heartbeat_ms_) {
    query_attempts_ = 0; querying_ = false; manifest_attempts_ = 0;
  }
  if (!reporting_ && manifest_attempts_ >= 3 && !confirmed_ &&
      uint32_t(millis() - manifest_at_) >= heartbeat_ms_) {
    retained.expires = 0; retain(); query_attempts_ = 0; querying_ = false; manifest_attempts_ = 0;
  }
  service_manifest_();
  if (confirmed_ && (presence_dirty_ || (ready && uint32_t(millis() - heartbeat_at_) >= heartbeat_ms_)))
    presence_(ready);
}
void ThreadSedTransporter::loop() {
  if (is_failed() || shutdown_) return;
  Packet *p;
  for (unsigned k = 0; k < 4 && xQueueReceive(packets_, &p, 0) == pdTRUE; k++) {
    process_(*p); delete p;
  }
  service_report_ack_();
  if (shutdown_) return;
  service_presence_();
  if (!reporting_) return;
  const uint32_t elapsed = millis() - started_;
  if (elapsed >= timeout_ms_) {
    stage_();
    if (confirmed_ && target_valid_() && opened_) transmit_();
    else {
      ESP_LOGW(TAG, "Report %u timed out before state submission: socket=%s target=%s manifest=%s discovery_attempts=%u manifest_attempts=%u",
               unsigned(report_id_), YESNO(opened_), YESNO(target_valid_()), YESNO(confirmed_),
               unsigned(query_attempts_), unsigned(manifest_attempts_));
      summary_.errors++; finish_();
    }
    return;
  }
  if (phase_ == Phase::DELAY && elapsed >= acquisition_delay_) {
    phase_ = Phase::ACQUIRE;
    for (auto *e : sensors_) e->prepare_acquisition();
    ESP_LOGD(TAG, "Acquiring %u hardware components", unsigned(update_components_.size()));
    for (auto *c : update_components_) if (!c->is_failed()) c->update();
  }
  if (phase_ == Phase::ACQUIRE) {
    bool ready = true;
    for (auto *e : sensors_) ready &= e->source_ready();
    if (ready) { stage_(); phase_ = Phase::SEND; }
  }
  if (phase_ == Phase::SEND && confirmed_ && target_valid_() && opened_) transmit_();
  if (is_failed() && reporting_) { summary_.errors++; finish_(); }
}
void ThreadSedTransporter::on_shutdown() {
  if (!shutdown_) presence_(false,true);
  shutdown_ = true;
  if (opened_ && ot_ && ot_->is_lock_initialized() && esp_openthread_lock_acquire(0)) {
    if (auto *instance = esp_openthread_get_instance()) otUdpClose(instance, &socket_);
    esp_openthread_lock_release(); opened_ = false;
  }
}
void ThreadSedTransporter::dump_config() {
  ESP_LOGCONFIG(TAG, "Thread SED reporter '%s': manual acquisition, UDP only", node_.c_str());
  ESP_LOGCONFIG(TAG, "  Read-only=%u controls=%u timeout=%ums full report every=%u",
      unsigned(sensors_.size()),unsigned(controls_.size()),unsigned(timeout_ms_),unsigned(full_every_));
  for (auto *e : sensors_) e->dump_config();
}

void ThreadSedTransporter::command_(const cJSON *j) {
  if (!uint_value(item(j,"command_id"))) return;
  const uint32_t id = item(j,"command_id")->valuedouble;
  for (const auto &c : commands_) if (c.id == id) {
    send_(retained.address,retained.port,c.response); return;
  }
  const std::string entity = str(j,"entity"), operation = str(j,"operation");
  bool success = false; std::string error = "Unknown control entity";
  for (auto &c : controls_) if (c.id == entity) {
    if (!ready_()) error = "Node is not control-ready";
    else if (operation != (c.type == "button" ? "press" : "set")) error = "Invalid operation";
    else {
      success = c.execute(item(j,"value"));
      error = success ? "" : "Invalid value or source rejected control";
    }
    break;
  }
  auto *response = envelope_("command_result");
  cJSON_AddNumberToObject(response,"command_id",id); cJSON_AddBoolToObject(response,"success",success);
  if (!error.empty()) cJSON_AddStringToObject(response,"error",error.c_str());
  const std::string wire = encode(response); cJSON_Delete(response);
  // Results describe local dispatch; asynchronous hardware confirmation comes from later state reports.
  if (commands_.size() >= 32) commands_.erase(commands_.begin());
  commands_.push_back({id,wire});
  send_(retained.address,retained.port,wire);
}

}  // namespace esphome::thread_sed_transporter
