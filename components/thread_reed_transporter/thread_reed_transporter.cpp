#include "thread_reed_transporter.h"
#include "esphome/core/log.h"
#include "esp_openthread.h"
#include "esp_openthread_lock.h"
#include "esp_random.h"
#include "openthread/message.h"
#include "openthread/thread.h"
#include "openthread/ip6.h"
#include <cstring>
#include <new>
#include <cmath>
namespace esphome::thread_reed_transporter {
using namespace thread_entity_bridge;
static const char *const TAG="thread_reed_transporter";
static bool integer(const cJSON *v,uint32_t lo=0,uint32_t hi=UINT32_MAX) {
 return cJSON_IsNumber(v)&&std::isfinite(v->valuedouble)&&v->valuedouble>=lo&&v->valuedouble<=hi&&floor(v->valuedouble)==v->valuedouble;
}
void ThreadReedTransporter::setup() {
 queue_=xQueueCreate(12,sizeof(Packet *));if(!queue_) {mark_failed();return;}
 char buf[17];snprintf(buf,sizeof(buf),"%08x%08x",unsigned(esp_random()),unsigned(esp_random()));session_=buf;
 for(size_t i=0;i<entries_.size();i++) if(entries_[i].endpoint.subscribe)
  entries_[i].endpoint.subscribe([this,i](){entries_[i].generation++;entries_[i].dirty=true;});
 // Traits are read after all component setup, not while Python/codegen binds IDs.
 set_timeout("manifest",0,[this](){if(!manifest_()) mark_failed();});
}
void ThreadReedTransporter::receive_(void *ctx, otMessage *message, const otMessageInfo *info) {
  auto *self = static_cast<ThreadReedTransporter *>(ctx);
  if (self->shutdown_ || !self->queue_) return;
  const int length = otMessageGetLength(message) - otMessageGetOffset(message);
  if (length <= 0 || length > 1232) { self->dropped_++; return; }
  auto *p = new (std::nothrow) Packet;
  if (!p) { self->dropped_++; return; }
  p->peer = info->mPeerAddr; p->port = info->mPeerPort; p->size = length;
  if (otMessageRead(message, otMessageGetOffset(message), p->data, length) != length) { delete p; self->dropped_++; return; }
  p->data[length] = 0;
  if (xQueueSend(self->queue_, &p, 0) != pdTRUE) { delete p; self->dropped_++; }
}
bool ThreadReedTransporter::open_() {
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
bool ThreadReedTransporter::send_(const otIp6Address &address, uint16_t port, const std::string &payload) {
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

cJSON *ThreadReedTransporter::envelope_(const char *type) const {
 auto *j=cJSON_CreateObject();cJSON_AddNumberToObject(j,"v",1);cJSON_AddStringToObject(j,"type",type);
 cJSON_AddStringToObject(j,"node",node_.c_str());cJSON_AddStringToObject(j,"session",session_.c_str());
 cJSON_AddStringToObject(j,"name",name_.c_str());cJSON_AddStringToObject(j,"node_type","reed");
 cJSON_AddNumberToObject(j,"heartbeat_interval_ms",heartbeat_ms_);
 cJSON_AddBoolToObject(j,"control_ready",true);return j;
}
void ThreadReedTransporter::request_full_sync_() {
 // A receipt for an older snapshot must not clear a new synchronization request.
 for (auto &entry : entries_) {
  entry.generation++;
  entry.dirty = true;
 }
}
void ThreadReedTransporter::invalidate_(bool preserve_report) {
 target_=confirmed_=querying_=check_pending_=false;
 if (!preserve_report) pending_={};
 request_full_sync_();
}
void ThreadReedTransporter::discovery() {
 // Preserve the current frozen report and newer dirty source states.
 invalidate_(true);
 if (pending_.id && !pending_.recovering) {
  pending_.recovering = true;
  pending_.recovery_at = millis();
 }
 ESP_LOGD(TAG, "Gateway rediscovery requested");
 if (!shutdown_ && !is_failed() && open_()) discover_();
}
void ThreadReedTransporter::discover_() {
 if(querying_ && uint32_t(millis()-query_at_)<3000)return;
 querying_=true;query_at_=millis();request_=esp_random();otIp6Address group{};
 otIp6AddressFromString(group_.c_str(),&group);
 auto *j=cJSON_CreateObject();cJSON_AddNumberToObject(j,"v",1);cJSON_AddStringToObject(j,"type","discover");cJSON_AddNumberToObject(j,"request_id",request_);
 send_(group,discovery_port_,encode(j));cJSON_Delete(j);
}
void ThreadReedTransporter::presence_() {
 auto *j=envelope_("presence");cJSON_AddNumberToObject(j,"revision",revision_);cJSON_AddBoolToObject(j,"check_manifest",true);
 if (send_(peer_, port_, encode(j))) {
  heartbeat_at_ = millis();
 }
 cJSON_Delete(j);
}
bool ThreadReedTransporter::manifest_() {
 if(name_.empty()||name_.size()>96){ESP_LOGE(TAG,"Node name must fit 1..96 UTF-8 bytes");return false;}
 std::vector<std::string> descriptors;
 for(auto &e:entries_) {
  auto *j=e.endpoint.describe();
  if(!j || !validate_capabilities(e.endpoint.kind,field(j,"capabilities"))) {
   ESP_LOGE(TAG,"Invalid/oversized capabilities for %s",e.endpoint.id.c_str());cJSON_Delete(j);return false;
  }
  if(string(j,"name").empty()) cJSON_ReplaceItemInObjectCaseSensitive(j,"name",cJSON_CreateString(e.endpoint.id.c_str()));
  if(string(j,"name").size()>96){ESP_LOGE(TAG,"Entity name exceeds 96 UTF-8 bytes: %s",e.endpoint.id.c_str());cJSON_Delete(j);return false;}
  descriptors.push_back(encode(j));cJSON_Delete(j);
 }
 auto pack=[this](const std::vector<std::string> &page,unsigned part,unsigned parts){
  auto *j=envelope_("manifest");cJSON_AddNumberToObject(j,"revision",revision_);cJSON_AddNumberToObject(j,"part",part);cJSON_AddNumberToObject(j,"parts",parts);
  auto *a=cJSON_AddArrayToObject(j,"entities");for(auto &d:page)cJSON_AddItemToArray(a,cJSON_Parse(d.c_str()));auto payload=encode(j);cJSON_Delete(j);return payload;
 };
 std::vector<std::vector<std::string>> pages(1);
 for(auto &d:descriptors) {
  pages.back().push_back(d);
  if(pages.back().size()>32 || pack(pages.back(),15,16).size()>1232) {
   pages.back().pop_back();if(pages.back().empty()||pages.size()>=16)return false;
   pages.push_back({d});if(pack(pages.back(),15,16).size()>1232)return false;
  }
 }
 manifest_pages_.clear();for(size_t k=0;k<pages.size();k++)manifest_pages_.push_back(pack(pages[k],k,pages.size()));
 return true;
}
void ThreadReedTransporter::process_(const Packet &p) {
 auto *j=cJSON_ParseWithLengthOpts(p.data,p.size+1,nullptr,true);
 if(!cJSON_IsObject(j)||!integer(field(j,"v"),1,1)){cJSON_Delete(j);return;}
 const auto type=string(j,"type");
 if(type=="offer") {
  auto gateway=string(j,"gateway_id");otIp6Address address{};
  if(querying_ && p.port==discovery_port_ && integer(field(j,"request_id")) && uint32_t(field(j,"request_id")->valuedouble)==request_ &&
   !gateway.empty() && gateway.size()<=32 && (expected_gateway_.empty()||expected_gateway_==gateway) &&
   integer(field(j,"port"),1,65535)&&integer(field(j,"ttl_s"),1,86400)&&
   otIp6AddressFromString(string(j,"address").c_str(),&address)==OT_ERROR_NONE && memcmp(&address,&p.peer,sizeof(address))==0) {
    if(gateway_!=gateway)results_.clear();
    gateway_=gateway;peer_=address;port_=field(j,"port")->valuedouble;ttl_ms_=uint32_t(field(j,"ttl_s")->valuedouble)*1000;
    target_at_=last_reply_=millis();target_=true;querying_=false;confirmed_=false;check_pending_=true;presence_();
    ESP_LOGI(TAG,"Gateway %s discovered; checking manifest revision %u",gateway_.c_str(),unsigned(revision_));
  }
  cJSON_Delete(j);return;
 }
 if(!target_||p.port!=port_||memcmp(&p.peer,&peer_,sizeof(peer_))||string(j,"gateway_id")!=gateway_||string(j,"node")!=node_||string(j,"session")!=session_){cJSON_Delete(j);return;}
 last_reply_=millis();
 if(type=="manifest_request") {confirmed_=false;check_pending_=false;manifest_at_=millis()-1000;manifest_next_=0;}
 else if(type=="manifest_ack" && integer(field(j,"revision"),revision_,revision_)) {
  if(cJSON_IsTrue(field(j,"accepted"))&&cJSON_IsTrue(field(j,"complete"))) {
   if (!confirmed_ || cJSON_IsTrue(field(j, "request_state"))) {
    request_full_sync_();
    ESP_LOGD(TAG, "Full state synchronization requested");
   }
   if(!confirmed_) ESP_LOGI(TAG,"Manifest synchronized; continuous transport ready");
   confirmed_=true;check_pending_=false;
   target_at_=millis(); // Valid manifest/heartbeat confirmation renews the offer TTL.
  } else if(cJSON_IsFalse(field(j,"accepted"))) {
   ESP_LOGW(TAG,"Manifest rejected: %s",string(j,"error").c_str());manifest_at_=millis()+2000;
  }
 } else if(type=="report_ack" && pending_.id && integer(field(j,"report_id"),pending_.id,pending_.id) &&
   integer(field(j,"parts"),pending_.pages.size(),pending_.pages.size())&&cJSON_IsTrue(field(j,"complete"))) {
  for(auto &s:pending_.samples)if(entries_[s.index].generation==s.generation)entries_[s.index].dirty=false;
  target_at_=millis(); // Only a matching complete report ACK renews the lease.
  pending_={};status_clear_warning();
 } else if(type=="command")command_(j);
 cJSON_Delete(j);
}
void ThreadReedTransporter::report_() {
 Pending next;next.id=++report_id_;if(!next.id)next.id=++report_id_;
 auto make=[&](){auto *j=envelope_("state");cJSON_AddNumberToObject(j,"seq",UINT32_MAX);cJSON_AddBoolToObject(j,"need_ack",true);cJSON_AddNumberToObject(j,"report_id",next.id);cJSON_AddNumberToObject(j,"part",15);cJSON_AddNumberToObject(j,"parts",16);cJSON_AddObjectToObject(j,"values");return j;};
 auto *j=make();auto *values=cJSON_GetObjectItemCaseSensitive(j,"values");
 bool valid=true;
 for(size_t i=0;i<entries_.size();i++)if(entries_[i].dirty) {
  auto *v=entries_[i].endpoint.read();if(!v)continue;
  auto *descriptor=entries_[i].endpoint.describe();
  const bool valid_value=validate_value(entries_[i].endpoint.kind,field(descriptor,"capabilities"),v,false);
  cJSON_Delete(descriptor);
  if(!valid_value) {ESP_LOGW(TAG,"Invalid/oversized state for %s; waiting for next source update",entries_[i].endpoint.id.c_str());cJSON_Delete(v);entries_[i].dirty=false;continue;}
  cJSON_AddItemToObject(values,entries_[i].endpoint.id.c_str(),v);
  if(encode(j).size()>1232) {
   v=cJSON_DetachItemFromObjectCaseSensitive(values,entries_[i].endpoint.id.c_str());
   if(!values->child||next.pages.size()>=15){cJSON_Delete(v);valid=false;break;}
   next.pages.push_back(encode(j));cJSON_Delete(j);j=make();values=cJSON_GetObjectItemCaseSensitive(j,"values");
   cJSON_AddItemToObject(values,entries_[i].endpoint.id.c_str(),v);
   if(encode(j).size()>1232){valid=false;break;}
  }
  next.samples.push_back({i,entries_[i].generation});
 }
 if (values->child) {
  next.pages.push_back(encode(j));
 }
 cJSON_Delete(j);
 if(!valid){ESP_LOGW(TAG,"State exceeds bounded UDP packet/page limits");status_set_warning();return;}
 if(next.pages.empty())return;
 for(size_t k=0;k<next.pages.size();k++) {
  auto *page=cJSON_Parse(next.pages[k].c_str());cJSON_SetNumberValue(cJSON_GetObjectItemCaseSensitive(page,"seq"),++sequence_);
  cJSON_SetNumberValue(cJSON_GetObjectItemCaseSensitive(page,"part"),k);cJSON_SetNumberValue(cJSON_GetObjectItemCaseSensitive(page,"parts"),next.pages.size());
  next.pages[k]=encode(page);cJSON_Delete(page);
 }
 next.at=millis();next.send_at=millis()-25;pending_=std::move(next);
}
void ThreadReedTransporter::command_(const cJSON *j) {
 if(!integer(field(j,"command_id"),1))return;
 uint32_t id=field(j,"command_id")->valuedouble;
 for(auto &r:results_)if(r.id==id){send_(peer_,port_,r.payload);return;}
 auto entity=string(j,"entity"),operation=string(j,"operation");bool success=false;std::string error="Unknown entity";cJSON *state=nullptr;
 for(auto &e:entries_)if(e.endpoint.id==entity) {
  auto *descriptor=e.endpoint.describe();auto *caps=field(descriptor,"capabilities");
  if(e.endpoint.kind=="numeric"||e.endpoint.kind=="binary"||e.endpoint.kind=="text")error="Read-only entity";
  else if(operation!=(e.endpoint.kind=="button"?"press":"set"))error="Invalid operation";
  else if(!validate_value(e.endpoint.kind,caps,field(j,"value"),true))error="Unsupported or invalid control value";
  else {success=e.endpoint.execute(field(j,"value"));error=success?"":"Control rejected";e.dirty=true;e.generation++;state=e.endpoint.read();}
  cJSON_Delete(descriptor);break;
 }
 auto *reply=envelope_("command_result");cJSON_AddStringToObject(reply,"gateway_id",gateway_.c_str());cJSON_AddNumberToObject(reply,"command_id",id);
 cJSON_AddBoolToObject(reply,"success",success);if(!error.empty())cJSON_AddStringToObject(reply,"error",error.c_str());if(state){cJSON_AddItemToObject(reply,"value",state);cJSON_AddNumberToObject(reply,"state_seq",++sequence_);}
 std::string payload=encode(reply);cJSON_Delete(reply);
 if (results_.size() >= 32) {
  results_.erase(results_.begin());
 }
 results_.push_back({id, payload});
 send_(peer_, port_, payload);
}
void ThreadReedTransporter::loop() {
 if(shutdown_||is_failed())return;
 Packet *p;for(unsigned k=0;k<6&&xQueueReceive(queue_,&p,0)==pdTRUE;k++){process_(*p);delete p;}
 if(!open_())return;
 const uint32_t now=millis();
 if (pending_.id && pending_.recovering && uint32_t(now-pending_.recovery_at)>=10000) {
  ESP_LOGW(TAG,"Report %u rediscovery timed out",unsigned(pending_.id));
  status_set_warning();invalidate_();return;
 }
 if(target_&&(uint32_t(now-target_at_)>=ttl_ms_||uint32_t(now-last_reply_)>=heartbeat_ms_*3))discovery();
 if(!target_){discover_();return;}
 if(uint32_t(now-heartbeat_at_)>=heartbeat_ms_)presence_();
 if(!confirmed_) {
  if(check_pending_ && uint32_t(now-heartbeat_at_)>=1000)check_pending_=false;
  if(!check_pending_ && !manifest_pages_.empty() && int32_t(now-manifest_at_)>=1000 && uint32_t(now-manifest_send_at_)>=25) {
   send_(peer_,port_,manifest_pages_[manifest_next_++]);manifest_send_at_=now;
   if(manifest_next_==manifest_pages_.size()){manifest_next_=0;manifest_at_=now;}
  }
  return;
 }
 if(uint32_t(now-sync_at_)>=sync_ms_){request_full_sync_();sync_at_=now;}
 if(pending_.id) {
  if (pending_.recovering) {
   pending_.recovering=false;pending_.retries=0;pending_.next_page=0;
   pending_.at=now;pending_.send_at=now-25;
   ESP_LOGD(TAG,"Report %u resubmitting frozen round after rediscovery",unsigned(pending_.id));
  }
  if(pending_.next_page<pending_.pages.size()) {
   if(uint32_t(now-pending_.send_at)>=25) {
    send_(peer_,port_,pending_.pages[pending_.next_page++]);pending_.send_at=now;
    if(pending_.next_page==pending_.pages.size())pending_.at=now;
   }
  } else if(uint32_t(now-pending_.at)>=ack_ms_) {
   if(pending_.retries>=resend_) {
    status_set_warning();
    if (!pending_.recovered) {
     pending_.recovered=true;
     ESP_LOGW(TAG,"Report %u ACK attempts exhausted; rediscovering once",unsigned(pending_.id));
     discovery();
    } else {
     ESP_LOGW(TAG,"Report %u ACK failed after rediscovery",unsigned(pending_.id));
     invalidate_();
    }
    return;
   }
   pending_.retries++;pending_.at=now;pending_.next_page=0;pending_.send_at=now-25;
  }
 } else if(uint32_t(now-batch_at_)>=batch_ms_){batch_at_=now;report_();}
}
void ThreadReedTransporter::on_shutdown() {
 shutdown_=true;
 if(opened_&&ot_->is_lock_initialized()&&esp_openthread_lock_acquire(0)) {
  if (auto *instance = esp_openthread_get_instance()) {
   otUdpClose(instance, &socket_);
  }
  esp_openthread_lock_release();
  opened_ = false;
 }
}
void ThreadReedTransporter::dump_config() {
 ESP_LOGCONFIG(TAG,"Thread UDP transporter '%s': %u existing entities, manifest revision %u",node_.c_str(),unsigned(entries_.size()),unsigned(revision_));
}
}
