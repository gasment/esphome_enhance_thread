#include "entity_bridge.h"
#include "esphome/core/log.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#ifdef USE_LIGHT
#include "esphome/components/light/light_output.h"
#include "esphome/components/light/light_effect.h"
#endif
namespace esphome::thread_entity_bridge {
// Suppress boot defaults until the remote node has supplied its first valid state.
struct RemoteMetadata {EntityBase *entity;bool ready;std::array<std::string,3> fields;};
static std::vector<RemoteMetadata> remote_states;
void register_remote(EntityBase *entity,const std::string &dc,const std::string &unit,const std::string &icon) {remote_states.push_back({entity,false,{dc,unit,icon}});}
void mark_remote_ready(EntityBase *entity) {for(auto &entry:remote_states)if(entry.entity==entity){entry.ready=true;return;}}
bool remote_state_ready(EntityBase *entity) {for(auto &entry:remote_states)if(entry.entity==entity)return entry.ready;return true;}
StringRef remote_metadata(EntityBase *entity,uint8_t category,StringRef fallback) {
  for (auto &entry : remote_states) {
    if (entry.entity == entity && category < entry.fields.size()) {
      return StringRef(entry.fields[category].c_str());
    }
  }
  return fallback;
}
const cJSON *field(const cJSON *j, const char *k) { return cJSON_GetObjectItemCaseSensitive(j,k); }
std::string string(const cJSON *j,const char *k,const std::string &fallback) {
  auto *v=field(j,k); return cJSON_IsString(v) && v->valuestring ? v->valuestring : fallback;
}
std::string encode(const cJSON *j) {
  char *raw=cJSON_PrintUnformatted(j); if(!raw) return {};
  std::string s(raw); cJSON_free(raw); return s;
}
static double num(const cJSON *j,const char *k,double fallback=0) {
  auto *v=field(j,k); return cJSON_IsNumber(v) && std::isfinite(v->valuedouble) ? v->valuedouble : fallback;
}
static bool yes(const cJSON *j,const char *k) { return cJSON_IsTrue(field(j,k)); }
static void n(cJSON *j,const char *k,double v) { if(std::isfinite(v)) cJSON_AddNumberToObject(j,k,v); }
static void b(cJSON *j,const char *k,bool v) { cJSON_AddBoolToObject(j,k,v); }
static void put_string(cJSON *j,const char *k,const char *v) { cJSON_AddStringToObject(j,k,v ? v : ""); }
template<class T> static void optional_n(cJSON *j,const char *k,const optional<T> &v) { if(v) n(j,k,static_cast<double>(*v)); }
template<class T> static void enum_list(cJSON *j,const char *k,const T &values) {
  auto *a=cJSON_AddArrayToObject(j,k); for(auto v:values) cJSON_AddItemToArray(a,cJSON_CreateNumber(static_cast<int>(v)));
}
template<class T> static void strings(cJSON *j,const char *k,const T &values) {
  auto *a=cJSON_AddArrayToObject(j,k); for(auto v:values) cJSON_AddItemToArray(a,cJSON_CreateString(v));
}
#if defined(USE_THREAD_GATEWAY) && (defined(USE_SELECT) || defined(USE_FAN) || defined(USE_CLIMATE) || defined(USE_LIGHT))
static std::vector<std::string> read_strings(const cJSON *j,const char *k) {
  std::vector<std::string> out;
  const cJSON *v;
  cJSON_ArrayForEach(v, field(j, k)) {
    if (cJSON_IsString(v)) {
      out.emplace_back(v->valuestring);
    }
  }
  return out;
}
#endif
#if defined(USE_THREAD_GATEWAY) && (defined(USE_FAN) || defined(USE_CLIMATE))
static std::vector<const char *> pointers(const std::vector<std::string> &values) {
  std::vector<const char *> out; for(auto &v:values) out.push_back(v.c_str()); return out;
}
#endif
static cJSON *base(EntityBase *e,const std::string &id,const char *kind) {
  auto *j=cJSON_CreateObject(); put_string(j,"id",id.c_str()); put_string(j,"type",kind); put_string(j,"name",e->get_name().c_str());
  const auto category=e->get_entity_category();
  if(category==ENTITY_CATEGORY_CONFIG) put_string(j,"entity_category","config");
  if(category==ENTITY_CATEGORY_DIAGNOSTIC) put_string(j,"entity_category","diagnostic");
  if(e->is_disabled_by_default()) b(j,"disabled_by_default",true);
  std::array<char,MAX_DEVICE_CLASS_LENGTH> dc{}; std::array<char,MAX_ICON_LENGTH> icon{};
  const char *d=e->get_device_class_to(dc), *i=e->get_icon_to(icon);
  if (d && *d) {
    put_string(j, "device_class", d);
  }
  if (i && *i) {
    put_string(j, "icon", i);
  }
  auto unit=e->get_unit_of_measurement_ref(); if(!unit.empty()) put_string(j,"unit",unit.c_str());
  return j;
}
static bool contains(const cJSON *a,const cJSON *v) {
  const cJSON *x; cJSON_ArrayForEach(x,a) if(cJSON_Compare(x,v,true)) return true; return false;
}
static bool finite_number(const cJSON *v,double lo=-3.402823466e38,double hi=3.402823466e38,bool whole=false) {
 return cJSON_IsNumber(v)&&std::isfinite(v->valuedouble)&&v->valuedouble>=lo&&v->valuedouble<=hi&&(!whole||floor(v->valuedouble)==v->valuedouble);
}
static bool valid_list(const cJSON *a,bool numeric,bool required=false) {
 if(!cJSON_IsArray(a)||cJSON_GetArraySize(a)>32||(required&&!a->child))return false;
 for(const cJSON *v=a->child;v;v=v->next) {
  if(numeric ? !finite_number(v,0,65535,true) : (!cJSON_IsString(v)||!v->valuestring||strlen(v->valuestring)>64))return false;
  for(const cJSON *other=v->next;other;other=other->next)if(cJSON_Compare(v,other,true))return false;
 }
 return true;
}
bool validate_capabilities(const std::string &kind,const cJSON *c) {
 if(!cJSON_IsObject(c)||encode(c).size()>900)return false;
 const cJSON *v;
 cJSON_ArrayForEach(v,c) {
  if(cJSON_IsNumber(v)&&!finite_number(v))return false;
  if(!(cJSON_IsNumber(v)||cJSON_IsBool(v)||cJSON_IsString(v)||cJSON_IsArray(v)))return false;
  if(cJSON_IsString(v)&&(!v->valuestring||strlen(v->valuestring)>255))return false;
 }
 if(kind=="number")return finite_number(field(c,"min"))&&finite_number(field(c,"max"))&&num(c,"min")<=num(c,"max")&&finite_number(field(c,"step"),1e-30)&&finite_number(field(c,"mode"),0,2,true);
 if(kind=="select")return valid_list(field(c,"options"),false,true);
 if(kind=="input_text")return finite_number(field(c,"min_length"),0,255,true)&&finite_number(field(c,"max_length"),1,255,true)&&num(c,"min_length")<=num(c,"max_length")&&finite_number(field(c,"mode"),0,1,true);
 if(kind=="fan")return cJSON_IsBool(field(c,"speed"))&&cJSON_IsBool(field(c,"direction"))&&cJSON_IsBool(field(c,"oscillation"))&&finite_number(field(c,"speed_count"),0,255,true)&&(!yes(c,"speed")||num(c,"speed_count")>=1)&&valid_list(field(c,"preset_modes"),false);
 if(kind=="light")return valid_list(field(c,"color_modes"),true,true)&&valid_list(field(c,"effects"),false)&&finite_number(field(c,"min_mireds"),0)&&finite_number(field(c,"max_mireds"),num(c,"min_mireds"));
 if(kind=="cover"||kind=="valve")return cJSON_IsBool(field(c,"position"))&&cJSON_IsBool(field(c,"stop"))&&cJSON_IsBool(field(c,"toggle"))&&(kind!="cover"||cJSON_IsBool(field(c,"tilt")));
 if(kind=="lock")return cJSON_IsBool(field(c,"open"))&&cJSON_IsBool(field(c,"requires_code"))&&finite_number(field(c,"states"),0,255,true);
 if(kind=="climate") {
  for(auto *key:{"modes","fan_modes","swing_modes","presets"})if(!valid_list(field(c,key),true))return false;
  for(auto *key:{"custom_fan_modes","custom_presets"})if(!valid_list(field(c,key),false))return false;
  return finite_number(field(c,"feature_flags"),0,UINT32_MAX,true)&&finite_number(field(c,"visual_min_temperature"))&&finite_number(field(c,"visual_max_temperature"),num(c,"visual_min_temperature"))&&finite_number(field(c,"visual_target_temperature_step"),1e-6)&&finite_number(field(c,"visual_current_temperature_step"),1e-6);
 }
 return kind=="numeric"||kind=="binary"||kind=="text"||kind=="switch"||kind=="button";
}
bool validate_value(const std::string &kind,const cJSON *c,const cJSON *v,bool command) {
 if(kind=="button")return !v||cJSON_IsNull(v);
 if(kind=="numeric"||kind=="number") {
  if(!finite_number(v))return false;
  return !command||kind=="numeric"||(v->valuedouble>=num(c,"min")&&v->valuedouble<=num(c,"max"));
 }
 if(kind=="binary"||kind=="switch")return cJSON_IsBool(v);
 if(kind=="text"||kind=="input_text"||kind=="select") {
  if(!cJSON_IsString(v)||!v->valuestring||strlen(v->valuestring)>255)return false;
  if(kind=="select")return contains(field(c,"options"),v);
  return kind!="input_text"||!command||(strlen(v->valuestring)>=num(c,"min_length")&&strlen(v->valuestring)<=num(c,"max_length"));
 }
 if(!cJSON_IsObject(v)||!v->child||encode(v).size()>900)return false;
 const cJSON *x;
 cJSON_ArrayForEach(x,v) {
  const std::string key=x->string?x->string:"";
  if(key.empty())return false;
  bool ok=false;
  if(kind=="fan") {
   if(key=="state")ok=cJSON_IsBool(x);
   if(key=="oscillating")ok=cJSON_IsBool(x)&&(!command||yes(c,"oscillation"));
   if(key=="speed")ok=finite_number(x,0,num(c,"speed_count"),true)&&(!command||yes(c,"speed"));
   if(key=="direction")ok=finite_number(x,0,1,true)&&(!command||yes(c,"direction"));
   if(key=="preset_mode")ok=cJSON_IsString(x)&&(!command||contains(field(c,"preset_modes"),x));
  } else if(kind=="cover"||kind=="valve") {
   if(key=="position")ok=finite_number(x,0,1)&&(!command||yes(c,"position")||x->valuedouble==0||x->valuedouble==1);
   if(key=="tilt"&&kind=="cover")ok=finite_number(x,0,1)&&(!command||yes(c,"tilt"));
   if(key=="stop"||key=="toggle")ok=command&&cJSON_IsBool(x)&&yes(c,key.c_str());
   if(key=="current_operation")ok=!command&&finite_number(x,0,2,true);
  } else if(kind=="lock") {
   if(key=="open")ok=command&&cJSON_IsBool(x)&&yes(c,"open");
   if(key=="state")ok=finite_number(x,0,7,true)&&(!command||x->valuedouble==1||x->valuedouble==2);
  } else if(kind=="light") {
   if(key=="state")ok=cJSON_IsBool(x);
   if(key=="color_mode")ok=finite_number(x,0,65535,true)&&((!command&&x->valuedouble==0)||contains(field(c,"color_modes"),x));
   if(key=="effect")ok=cJSON_IsString(x)&&(!command||std::string(x->valuestring)=="None"||contains(field(c,"effects"),x));
   if(key=="transition_length"||key=="flash_length")ok=command&&finite_number(x,0,3600000,true);
   if(key=="color_temperature")ok=finite_number(x,0)&&(!command||(x->valuedouble>=num(c,"min_mireds")&&x->valuedouble<=num(c,"max_mireds")));
   for(auto *unit:{"brightness","color_brightness","red","green","blue","white","cold_white","warm_white"})if(key==unit)ok=finite_number(x,0,1);
   if(command && ok) {
    unsigned capability=key=="brightness"||key=="transition_length"?2:key=="white"?4:key=="color_temperature"?8:key=="cold_white"||key=="warm_white"?16:key=="red"||key=="green"||key=="blue"||key=="color_brightness"?32:0;
    if(capability) {bool supported=false;const cJSON *mode;cJSON_ArrayForEach(mode,field(c,"color_modes"))supported|=(unsigned(mode->valuedouble)&capability)!=0;ok=supported;}
   }
  } else if(kind=="climate") {
   for(auto *k:{"target_temperature","target_temperature_low","target_temperature_high"})if(key==k)ok=finite_number(x)&&(!command||(x->valuedouble>=num(c,"visual_min_temperature")&&x->valuedouble<=num(c,"visual_max_temperature")));
   if(key=="target_humidity")ok=finite_number(x,0,100);
   for(auto *k:{"current_temperature","current_humidity"})if(key==k)ok=!command&&finite_number(x);
   if(key=="action")ok=!command&&finite_number(x,0,16,true);
   const char *list=key=="mode"?"modes":key=="fan_mode"?"fan_modes":key=="swing_mode"?"swing_modes":key=="preset"?"presets":nullptr;
   if(list)ok=finite_number(x,0,255,true)&&(!command||contains(field(c,list),x));
   if(key=="custom_fan_mode")ok=cJSON_IsString(x)&&contains(field(c,"custom_fan_modes"),x);
   if(key=="custom_preset")ok=cJSON_IsString(x)&&contains(field(c,"custom_presets"),x);
   if(command && ok) {
    const unsigned features=unsigned(num(c,"feature_flags"));
    if(key=="target_temperature" && (features&4))ok=false;
    if((key=="target_temperature_low"||key=="target_temperature_high") && !(features&6))ok=false;
    if(key=="target_humidity" && !(features&16))ok=false;
   }
  }
  if(!ok)return false;
 }
 if(command&&kind=="climate"&&((field(v,"fan_mode")&&field(v,"custom_fan_mode"))||(field(v,"preset")&&field(v,"custom_preset"))))return false;
 if(command&&kind=="climate"&&field(v,"target_temperature_low")&&field(v,"target_temperature_high")&&num(v,"target_temperature_low")>num(v,"target_temperature_high"))return false;
 return true;
}

#ifdef USE_SENSOR
Endpoint bind(const std::string &id,sensor::Sensor *s) {
 Endpoint e{id,"numeric",s};
 e.describe=[s,id]() { auto *j=base(s,id,"numeric"); auto *c=cJSON_AddObjectToObject(j,"capabilities"); n(j,"accuracy_decimals",s->get_accuracy_decimals()); b(j,"force_update",s->get_force_update());
 const char *sc=""; switch(s->get_state_class()) {case sensor::STATE_CLASS_MEASUREMENT:sc="measurement";break;case sensor::STATE_CLASS_TOTAL:sc="total";break;case sensor::STATE_CLASS_TOTAL_INCREASING:sc="total_increasing";break;case sensor::STATE_CLASS_MEASUREMENT_ANGLE:sc="measurement_angle";break;default:break;} if(*sc) put_string(j,"state_class",sc); return j; };
 e.read=[s]() -> cJSON * { return s->has_state() && std::isfinite(s->state) ? cJSON_CreateNumber(s->state) : nullptr; };
 e.execute=[s](const cJSON *v) { return false; };
 e.subscribe=[s](std::function<void()> cb) { s->add_on_state_callback([cb](auto &&...) { cb(); }); };
 return e;
}
#endif

#ifdef USE_BINARY_SENSOR
Endpoint bind(const std::string &id,binary_sensor::BinarySensor *s) {
 Endpoint e{id,"binary",s};
 e.describe=[s,id]() { auto *j=base(s,id,"binary"); cJSON_AddObjectToObject(j,"capabilities"); return j; };
 e.read=[s]() -> cJSON * { return s->has_state() ? cJSON_CreateBool(s->state) : nullptr; };
 e.execute=[s](const cJSON *v) { return false; };
 e.subscribe=[s](std::function<void()> cb) { s->add_on_state_callback([cb](auto &&...) { cb(); }); };
 return e;
}
#endif

#ifdef USE_TEXT_SENSOR
Endpoint bind(const std::string &id,text_sensor::TextSensor *s) {
 Endpoint e{id,"text",s};
 e.describe=[s,id]() { auto *j=base(s,id,"text"); auto *c=cJSON_AddObjectToObject(j,"capabilities"); n(j,"max_length",255); return j; };
 e.read=[s]() -> cJSON * { return s->has_state() ? cJSON_CreateString(s->state.c_str()) : nullptr; };
 e.execute=[s](const cJSON *v) { return false; };
 e.subscribe=[s](std::function<void()> cb) { s->add_on_state_callback([cb](auto &&...) { cb(); }); };
 return e;
}
#endif

#ifdef USE_SWITCH
Endpoint bind(const std::string &id,switch_::Switch *s) {
 Endpoint e{id,"switch",s};
 e.describe=[s,id]() { auto *j=base(s,id,"switch"); auto *c=cJSON_AddObjectToObject(j,"capabilities"); b(c,"assumed_state",s->assumed_state()); return j; };
 // Match Native API switch state: Switch::publish_state() does not maintain
 // EntityBase::has_state(), unlike sensor/number/select/text publishers.
 e.read=[s]() -> cJSON * { return cJSON_CreateBool(s->state); };
 e.execute=[s](const cJSON *v) { s->control(cJSON_IsTrue(v)); return true; };
 e.subscribe=[s](std::function<void()> cb) { s->add_on_state_callback([cb](auto &&...) { cb(); }); };
 return e;
}
#endif

#ifdef USE_NUMBER
Endpoint bind(const std::string &id,number::Number *s) {
 Endpoint e{id,"number",s};
 e.describe=[s,id]() { auto *j=base(s,id,"number"); auto *c=cJSON_AddObjectToObject(j,"capabilities"); n(c,"min",s->traits.get_min_value()); n(c,"max",s->traits.get_max_value()); n(c,"step",s->traits.get_step()); n(c,"mode",s->traits.get_mode()); n(j,"min",s->traits.get_min_value()); n(j,"max",s->traits.get_max_value()); n(j,"step",s->traits.get_step()); return j; };
 e.read=[s]() -> cJSON * { return s->has_state() && std::isfinite(s->state) ? cJSON_CreateNumber(s->state) : nullptr; };
 e.execute=[s](const cJSON *v) { s->make_call().set_value(v->valuedouble).perform(); return true; };
 e.subscribe=[s](std::function<void()> cb) { s->add_on_state_callback([cb](auto &&...) { cb(); }); };
 return e;
}
#endif

#ifdef USE_SELECT
Endpoint bind(const std::string &id,select::Select *s) {
 Endpoint e{id,"select",s};
 e.describe=[s,id]() { auto *j=base(s,id,"select"); auto *c=cJSON_AddObjectToObject(j,"capabilities"); strings(c,"options",s->traits.get_options()); strings(j,"options",s->traits.get_options()); return j; };
 e.read=[s]() -> cJSON * { return s->has_state() ? cJSON_CreateString(s->current_option().c_str()) : nullptr; };
 e.execute=[s](const cJSON *v) { s->make_call().set_option(v->valuestring).perform(); return true; };
 e.subscribe=[s](std::function<void()> cb) { s->add_on_state_callback([cb](auto &&...) { cb(); }); };
 return e;
}
#endif

#ifdef USE_BUTTON
Endpoint bind(const std::string &id,button::Button *s) {
 Endpoint e{id,"button",s};
 e.describe=[s,id]() { auto *j=base(s,id,"button"); auto *c=cJSON_AddObjectToObject(j,"capabilities");  return j; };
 e.read=[s]() -> cJSON * { return nullptr; };
 e.execute=[s](const cJSON *v) { s->press(); return true; };
 return e;
}
#endif

#ifdef USE_TEXT
Endpoint bind(const std::string &id,text::Text *s) {
 Endpoint e{id,"input_text",s};
 e.describe=[s,id]() { auto *j=base(s,id,"input_text"); auto *c=cJSON_AddObjectToObject(j,"capabilities"); n(c,"min_length",s->traits.get_min_length()); n(c,"max_length",s->traits.get_max_length()); n(c,"mode",s->traits.get_mode()); put_string(c,"pattern",s->traits.get_pattern_c_str()); n(j,"max_length",s->traits.get_max_length()); return j; };
 e.read=[s]() -> cJSON * { return s->has_state() ? cJSON_CreateString(s->state.c_str()) : nullptr; };
 e.execute=[s](const cJSON *v) { s->make_call().set_value(v->valuestring).perform(); return true; };
 e.subscribe=[s](std::function<void()> cb) { s->add_on_state_callback([cb](auto &&...) { cb(); }); };
 return e;
}
#endif

#ifdef USE_FAN
static cJSON *caps(fan::Fan *s) { auto *j=cJSON_CreateObject(); auto t=s->get_traits();
 b(j,"oscillation",t.supports_oscillation());
 b(j,"speed",t.supports_speed());
 b(j,"direction",t.supports_direction());
 n(j,"speed_count",t.supported_speed_count());
 strings(j,"preset_modes",t.supported_preset_modes());
 return j; }
static cJSON *read(fan::Fan *s) { auto *j=cJSON_CreateObject();
 b(j,"state",s->state); b(j,"oscillating",s->oscillating); n(j,"speed",s->speed); n(j,"direction",static_cast<int>(s->direction)); put_string(j,"preset_mode",s->get_preset_mode().c_str());
 return j; }
static bool execute(fan::Fan *s,const cJSON *v) { if(!cJSON_IsObject(v)) return false; auto call=s->make_call();
 if(auto *x=field(v,"state")) call.set_state(cJSON_IsTrue(x));
 if(auto *x=field(v,"oscillating")) call.set_oscillating(cJSON_IsTrue(x));
 if(auto *x=field(v,"speed")) call.set_speed(x->valuedouble);
 if(auto *x=field(v,"direction")) call.set_direction(static_cast<fan::FanDirection>(x->valuedouble));
 if(auto *x=field(v,"preset_mode")) call.set_preset_mode(x->valuestring);
 call.perform(); return true; }
Endpoint bind(const std::string &id,fan::Fan *s) { Endpoint e{id,"fan",s};
 e.describe=[s,id]() { auto *j=base(s,id,"fan"); cJSON_AddItemToObject(j,"capabilities",caps(s)); return j; };
 e.read=[s]() { return read(s); }; e.execute=[s](const cJSON *v) { return execute(s,v); };
 e.subscribe=[s](std::function<void()> cb) { s->add_on_state_callback([cb](auto &&...) { cb(); }); };
 return e; }
#endif

#ifdef USE_COVER
static cJSON *caps(cover::Cover *s) { auto *j=cJSON_CreateObject(); auto t=s->get_traits();
 b(j,"assumed_state",t.get_is_assumed_state());
 b(j,"position",t.get_supports_position());
 b(j,"tilt",t.get_supports_tilt());
 b(j,"stop",t.get_supports_stop());
 b(j,"toggle",t.get_supports_toggle());
 return j; }
static cJSON *read(cover::Cover *s) { auto *j=cJSON_CreateObject();
 n(j,"position",s->position); n(j,"current_operation",s->current_operation);
 n(j,"tilt",s->tilt);
 return j; }
static bool execute(cover::Cover *s,const cJSON *v) { if(!cJSON_IsObject(v)) return false; auto call=s->make_call();
 if(auto *x=field(v,"position")) call.set_position(x->valuedouble);
 if(auto *x=field(v,"tilt")) call.set_tilt(x->valuedouble);
 if(auto *x=field(v,"stop")) call.set_stop(cJSON_IsTrue(x));
 if(yes(v,"toggle")) call.set_command_toggle();
 call.perform(); return true; }
Endpoint bind(const std::string &id,cover::Cover *s) { Endpoint e{id,"cover",s};
 e.describe=[s,id]() { auto *j=base(s,id,"cover"); cJSON_AddItemToObject(j,"capabilities",caps(s)); return j; };
 e.read=[s]() { return read(s); }; e.execute=[s](const cJSON *v) { return execute(s,v); };
 e.subscribe=[s](std::function<void()> cb) { s->add_on_state_callback([cb](auto &&...) { cb(); }); };
 return e; }
#endif

#ifdef USE_VALVE
static cJSON *caps(valve::Valve *s) { auto *j=cJSON_CreateObject(); auto t=s->get_traits();
 b(j,"assumed_state",t.get_is_assumed_state());
 b(j,"position",t.get_supports_position());
 b(j,"stop",t.get_supports_stop());
 b(j,"toggle",t.get_supports_toggle());
 return j; }
static cJSON *read(valve::Valve *s) { auto *j=cJSON_CreateObject();
 n(j,"position",s->position); n(j,"current_operation",s->current_operation);
 return j; }
static bool execute(valve::Valve *s,const cJSON *v) { if(!cJSON_IsObject(v)) return false; auto call=s->make_call();
 if(auto *x=field(v,"position")) call.set_position(x->valuedouble);
 if(auto *x=field(v,"stop")) call.set_stop(cJSON_IsTrue(x));
 if(yes(v,"toggle")) call.set_command_toggle();
 call.perform(); return true; }
Endpoint bind(const std::string &id,valve::Valve *s) { Endpoint e{id,"valve",s};
 e.describe=[s,id]() { auto *j=base(s,id,"valve"); cJSON_AddItemToObject(j,"capabilities",caps(s)); return j; };
 e.read=[s]() { return read(s); }; e.execute=[s](const cJSON *v) { return execute(s,v); };
 e.subscribe=[s](std::function<void()> cb) { s->add_on_state_callback([cb](auto &&...) { cb(); }); };
 return e; }
#endif

#ifdef USE_LOCK
static cJSON *caps(lock::Lock *s) { auto *j=cJSON_CreateObject(); auto t=s->traits;
 b(j,"assumed_state",t.get_assumed_state());
 b(j,"open",t.get_supports_open());
 b(j,"requires_code",t.get_requires_code());
 n(j,"states",t.get_supported_states_mask());
 return j; }
static cJSON *read(lock::Lock *s) { auto *j=cJSON_CreateObject();
 n(j,"state",s->state);
 return j; }
static bool execute(lock::Lock *s,const cJSON *v) { if(!cJSON_IsObject(v)) return false; auto call=s->make_call();
 if(auto *x=field(v,"state")) call.set_state(static_cast<lock::LockState>(x->valuedouble));
 if(yes(v,"open")) { s->open(); return true; }
 call.perform(); return true; }
Endpoint bind(const std::string &id,lock::Lock *s) { Endpoint e{id,"lock",s};
 e.describe=[s,id]() { auto *j=base(s,id,"lock"); cJSON_AddItemToObject(j,"capabilities",caps(s)); return j; };
 e.read=[s]() { return read(s); }; e.execute=[s](const cJSON *v) { return execute(s,v); };
 e.subscribe=[s](std::function<void()> cb) { s->add_on_state_callback([cb](auto &&...) { cb(); }); };
 return e; }
#endif

#ifdef USE_LIGHT
static cJSON *caps(light::LightState *s) { auto *j=cJSON_CreateObject(); auto t=s->get_traits();
 n(j,"min_mireds",t.get_min_mireds());
 n(j,"max_mireds",t.get_max_mireds());
 enum_list(j,"color_modes",t.get_supported_color_modes()); auto *a=cJSON_AddArrayToObject(j,"effects"); for(auto *effect:s->get_effects()) cJSON_AddItemToArray(a,cJSON_CreateString(effect->get_name().c_str())); n(j,"default_transition_length",s->get_default_transition_length());
 return j; }
static cJSON *read(light::LightState *s) { auto *j=cJSON_CreateObject();
 auto &v=s->remote_values; b(j,"state",v.is_on()); n(j,"color_mode",static_cast<int>(v.get_color_mode())); put_string(j,"effect",s->get_effect_name().c_str());
 n(j,"brightness",v.get_brightness());
 n(j,"color_brightness",v.get_color_brightness());
 n(j,"red",v.get_red());
 n(j,"green",v.get_green());
 n(j,"blue",v.get_blue());
 n(j,"white",v.get_white());
 n(j,"color_temperature",v.get_color_temperature());
 n(j,"cold_white",v.get_cold_white());
 n(j,"warm_white",v.get_warm_white());
 return j; }
static bool execute(light::LightState *s,const cJSON *v) { if(!cJSON_IsObject(v)) return false; auto call=s->make_call();
 if(auto *x=field(v,"state")) call.set_state(cJSON_IsTrue(x));
 if(auto *x=field(v,"color_mode")) call.set_color_mode(static_cast<light::ColorMode>(x->valuedouble));
 if(auto *x=field(v,"brightness")) call.set_brightness(x->valuedouble);
 if(auto *x=field(v,"color_brightness")) call.set_color_brightness(x->valuedouble);
 if(auto *x=field(v,"red")) call.set_red(x->valuedouble);
 if(auto *x=field(v,"green")) call.set_green(x->valuedouble);
 if(auto *x=field(v,"blue")) call.set_blue(x->valuedouble);
 if(auto *x=field(v,"white")) call.set_white(x->valuedouble);
 if(auto *x=field(v,"color_temperature")) call.set_color_temperature(x->valuedouble);
 if(auto *x=field(v,"cold_white")) call.set_cold_white(x->valuedouble);
 if(auto *x=field(v,"warm_white")) call.set_warm_white(x->valuedouble);
 if(auto *x=field(v,"effect")) call.set_effect(x->valuestring);
 if(auto *x=field(v,"transition_length")) call.set_transition_length(x->valuedouble);
 if(auto *x=field(v,"flash_length")) call.set_flash_length(x->valuedouble);
 call.perform(); return true; }
class Listener : public light::LightRemoteValuesListener {
 public: explicit Listener(std::function<void()> cb):cb_(std::move(cb)) {} void on_light_remote_values_update() override { cb_(); }
 private: std::function<void()> cb_;
};
Endpoint bind(const std::string &id,light::LightState *s) { Endpoint e{id,"light",s};
 e.describe=[s,id]() { auto *j=base(s,id,"light"); cJSON_AddItemToObject(j,"capabilities",caps(s)); return j; };
 e.read=[s]() { return read(s); }; e.execute=[s](const cJSON *v) { return execute(s,v); };
 e.subscribe=[s](std::function<void()> cb) { s->add_remote_values_listener(new Listener(std::move(cb))); };
 return e; }
#endif

#ifdef USE_CLIMATE
static cJSON *caps(climate::Climate *s) { auto *j=cJSON_CreateObject(); auto t=s->get_traits();
 n(j,"feature_flags",t.get_feature_flags());
 n(j,"visual_min_temperature",t.get_visual_min_temperature());
 n(j,"visual_max_temperature",t.get_visual_max_temperature());
 n(j,"visual_target_temperature_step",t.get_visual_target_temperature_step());
 n(j,"visual_current_temperature_step",t.get_visual_current_temperature_step());
 n(j,"visual_min_humidity",t.get_visual_min_humidity());
 n(j,"visual_max_humidity",t.get_visual_max_humidity());
 enum_list(j,"modes",t.get_supported_modes());
 enum_list(j,"fan_modes",t.get_supported_fan_modes());
 enum_list(j,"swing_modes",t.get_supported_swing_modes());
 enum_list(j,"presets",t.get_supported_presets());
 strings(j,"custom_fan_modes",t.get_supported_custom_fan_modes());
 strings(j,"custom_presets",t.get_supported_custom_presets());
 n(j,"temperature_unit",static_cast<int>(t.get_temperature_unit()));
 return j; }
static cJSON *read(climate::Climate *s) { auto *j=cJSON_CreateObject();
 n(j,"current_temperature",s->current_temperature);
 n(j,"current_humidity",s->current_humidity);
 n(j,"target_humidity",s->target_humidity);
 n(j,"mode",s->mode);
 n(j,"action",s->action);
 n(j,"swing_mode",s->swing_mode);
 auto t=s->get_traits(); if(t.has_feature_flags(climate::CLIMATE_SUPPORTS_TWO_POINT_TARGET_TEMPERATURE | climate::CLIMATE_REQUIRES_TWO_POINT_TARGET_TEMPERATURE)) { n(j,"target_temperature_low",s->target_temperature_low); n(j,"target_temperature_high",s->target_temperature_high); } else n(j,"target_temperature",s->target_temperature);
 optional_n(j,"fan_mode",s->fan_mode); optional_n(j,"preset",s->preset); if(s->has_custom_fan_mode()) put_string(j,"custom_fan_mode",s->get_custom_fan_mode().c_str()); if(s->has_custom_preset()) put_string(j,"custom_preset",s->get_custom_preset().c_str());
 return j; }
static bool execute(climate::Climate *s,const cJSON *v) { if(!cJSON_IsObject(v)) return false; auto call=s->make_call();
 if(auto *x=field(v,"target_temperature")) call.set_target_temperature(x->valuedouble);
 if(auto *x=field(v,"target_temperature_low")) call.set_target_temperature_low(x->valuedouble);
 if(auto *x=field(v,"target_temperature_high")) call.set_target_temperature_high(x->valuedouble);
 if(auto *x=field(v,"target_humidity")) call.set_target_humidity(x->valuedouble);
 if(auto *x=field(v,"mode")) call.set_mode(static_cast<climate::ClimateMode>(x->valuedouble));
 if(auto *x=field(v,"fan_mode")) call.set_fan_mode(static_cast<climate::ClimateFanMode>(x->valuedouble));
 if(auto *x=field(v,"swing_mode")) call.set_swing_mode(static_cast<climate::ClimateSwingMode>(x->valuedouble));
 if(auto *x=field(v,"preset")) call.set_preset(static_cast<climate::ClimatePreset>(x->valuedouble));
 if(auto *x=field(v,"custom_fan_mode")) call.set_fan_mode(x->valuestring);
 if(auto *x=field(v,"custom_preset")) call.set_preset(x->valuestring);
 call.perform(); return true; }
Endpoint bind(const std::string &id,climate::Climate *s) { Endpoint e{id,"climate",s};
 e.describe=[s,id]() { auto *j=base(s,id,"climate"); cJSON_AddItemToObject(j,"capabilities",caps(s)); return j; };
 e.read=[s]() { return read(s); }; e.execute=[s](const cJSON *v) { return execute(s,v); };
 e.subscribe=[s](std::function<void()> cb) { s->add_on_state_callback([cb](auto &&...) { cb(); }); };
 return e; }
#endif

#ifdef USE_THREAD_GATEWAY
template<class T> class EntityProxy : public T, public Proxy {
 public:
  explicit EntityProxy(Sender sender):sender_(std::move(sender)) { this->entity=this; }
  void set_device(Device *d) override { this->set_device_(d); }
 protected: Sender sender_;
};
#ifdef USE_SWITCH
class RemoteSwitch : public EntityProxy<switch_::Switch> {
 public:
 RemoteSwitch(const cJSON *c,Sender sender):EntityProxy(std::move(sender)) {
 this->set_restore_mode(switch_::SWITCH_RESTORE_DISABLED); assumed_=yes(c,"assumed_state"); this->apply=[this](const cJSON *v){this->publish_state(cJSON_IsTrue(v));};
 }
 bool assumed_state() override {return assumed_;}
 protected:
 bool assumed_{false};
 void write_state(bool v) override { auto *j=cJSON_CreateBool(v); this->sender_(j); cJSON_Delete(j); }
};
#endif
#ifdef USE_NUMBER
class RemoteNumber : public EntityProxy<number::Number> {
 public:
 RemoteNumber(const cJSON *c,Sender sender):EntityProxy(std::move(sender)) {
 this->traits.set_min_value(num(c,"min")); this->traits.set_max_value(num(c,"max")); this->traits.set_step(num(c,"step",1)); this->traits.set_mode(static_cast<number::NumberMode>(int(num(c,"mode")))); this->apply=[this](const cJSON *v){this->publish_state(v->valuedouble);};
 }
 protected:
 void control(float v) override {auto *j=cJSON_CreateNumber(v); this->sender_(j); cJSON_Delete(j);}
};
#endif
#ifdef USE_SELECT
class RemoteSelect : public EntityProxy<select::Select> {
 public:
 RemoteSelect(const cJSON *c,Sender sender):EntityProxy(std::move(sender)) {
 values_=read_strings(c,"options"); FixedVector<const char *> options; options.init(values_.size()); for(auto &v:values_) options.push_back(v.c_str()); this->traits.set_options(options); this->apply=[this](const cJSON *v){this->publish_state(v->valuestring);};
 }
 protected:
 std::vector<std::string> values_;
 void control(const std::string &v) override {auto *j=cJSON_CreateString(v.c_str()); this->sender_(j); cJSON_Delete(j);}
};
#endif
#ifdef USE_BUTTON
class RemoteButton : public EntityProxy<button::Button> {
 public:
 RemoteButton(const cJSON *c,Sender sender):EntityProxy(std::move(sender)) {
 }
 protected:
 void press_action() override {this->sender_(nullptr);}
};
#endif
#ifdef USE_TEXT
class RemoteText : public EntityProxy<text::Text> {
 public:
 RemoteText(const cJSON *c,Sender sender):EntityProxy(std::move(sender)) {
 pattern_=string(c,"pattern"); this->traits.set_pattern(pattern_.c_str()); this->traits.set_min_length(num(c,"min_length")); this->traits.set_max_length(num(c,"max_length",128)); this->traits.set_mode(static_cast<text::TextMode>(int(num(c,"mode")))); this->apply=[this](const cJSON *v){this->publish_state(v->valuestring);};
 }
 protected:
 std::string pattern_;
 void control(const std::string &v) override {auto *j=cJSON_CreateString(v.c_str()); this->sender_(j); cJSON_Delete(j);}
};
#endif
#ifdef USE_FAN
class RemoteFan : public EntityProxy<fan::Fan> {
 public:
 RemoteFan(const cJSON *c,Sender sender):EntityProxy(std::move(sender)) {
 traits_.set_oscillation(yes(c,"oscillation"));
 traits_.set_speed(yes(c,"speed"));
 traits_.set_direction(yes(c,"direction"));
 traits_.set_supported_speed_count(num(c,"speed_count"));
 modes_=read_strings(c,"preset_modes"); this->set_supported_preset_modes(pointers(modes_));
 this->apply=[this](const cJSON *v) {
 this->state=yes(v,"state"); this->oscillating=yes(v,"oscillating"); this->speed=num(v,"speed"); this->direction=static_cast<fan::FanDirection>(int(num(v,"direction"))); auto preset=string(v,"preset_mode"); if(preset.empty()) this->clear_preset_mode_(); else this->set_preset_mode_(preset); this->publish_state();
 };
 }
 fan::FanTraits get_traits() override { this->wire_preset_modes_(traits_); return traits_; }
 protected:
 fan::FanTraits traits_;
 std::vector<std::string> modes_;
 void control(const fan::FanCall &call) override { auto *j=cJSON_CreateObject();
 if(call.get_state()) b(j,"state",*call.get_state());
 if(call.get_oscillating()) b(j,"oscillating",*call.get_oscillating());
 optional_n(j,"speed",call.get_speed());
 optional_n(j,"direction",call.get_direction());
 if(call.has_preset_mode()) put_string(j,"preset_mode",call.get_preset_mode());
 this->sender_(j); cJSON_Delete(j); }
};
#endif
#ifdef USE_COVER
class RemoteCover : public EntityProxy<cover::Cover> {
 public:
 RemoteCover(const cJSON *c,Sender sender):EntityProxy(std::move(sender)) {
 traits_.set_is_assumed_state(yes(c,"assumed_state"));
 traits_.set_supports_position(yes(c,"position"));
 traits_.set_supports_tilt(yes(c,"tilt"));
 traits_.set_supports_stop(yes(c,"stop"));
 traits_.set_supports_toggle(yes(c,"toggle"));
 this->apply=[this](const cJSON *v) {
 this->position=num(v,"position"); this->current_operation=static_cast<cover::CoverOperation>(int(num(v,"current_operation")));
 this->tilt=num(v,"tilt");
 this->publish_state(false);
 };
 }
 cover::CoverTraits get_traits() override { return traits_; }
 protected:
 cover::CoverTraits traits_;
 void control(const cover::CoverCall &call) override { auto *j=cJSON_CreateObject();
 optional_n(j,"position",call.get_position());
 optional_n(j,"tilt",call.get_tilt());
 if (call.get_stop()) {
   b(j, "stop", true);
 }
 if (call.get_toggle()) {
   b(j, "toggle", *call.get_toggle());
 }
 this->sender_(j); cJSON_Delete(j); }
};
#endif
#ifdef USE_VALVE
class RemoteValve : public EntityProxy<valve::Valve> {
 public:
 RemoteValve(const cJSON *c,Sender sender):EntityProxy(std::move(sender)) {
 traits_.set_is_assumed_state(yes(c,"assumed_state"));
 traits_.set_supports_position(yes(c,"position"));
 traits_.set_supports_stop(yes(c,"stop"));
 traits_.set_supports_toggle(yes(c,"toggle"));
 this->apply=[this](const cJSON *v) {
 this->position=num(v,"position"); this->current_operation=static_cast<valve::ValveOperation>(int(num(v,"current_operation")));
 this->publish_state(false);
 };
 }
 valve::ValveTraits get_traits() override { return traits_; }
 protected:
 valve::ValveTraits traits_;
 void control(const valve::ValveCall &call) override { auto *j=cJSON_CreateObject();
 optional_n(j,"position",call.get_position());
 if (call.get_stop()) {
   b(j, "stop", true);
 }
 if (call.get_toggle()) {
   b(j, "toggle", *call.get_toggle());
 }
 this->sender_(j); cJSON_Delete(j); }
};
#endif
#ifdef USE_LOCK
class RemoteLock : public EntityProxy<lock::Lock> {
 public:
 RemoteLock(const cJSON *c,Sender sender):EntityProxy(std::move(sender)) {
 this->traits.set_assumed_state(yes(c,"assumed_state"));
 this->traits.set_supports_open(yes(c,"open"));
 this->traits.set_requires_code(yes(c,"requires_code"));
 this->traits.set_supported_states_mask(num(c,"states"));
 this->apply=[this](const cJSON *v) {
 this->publish_state(static_cast<lock::LockState>(int(num(v,"state"))));
 };
 }
 protected:
 void control(const lock::LockCall &call) override { auto *j=cJSON_CreateObject();
 optional_n(j,"state",call.get_state());
 this->sender_(j); cJSON_Delete(j); }
 void open_latch() override { auto *j=cJSON_CreateObject(); b(j,"open",true); this->sender_(j); cJSON_Delete(j); }
};
#endif
#ifdef USE_CLIMATE
class RemoteClimate : public EntityProxy<climate::Climate> {
 public:
 RemoteClimate(const cJSON *c,Sender sender):EntityProxy(std::move(sender)) {
 traits_.set_feature_flags(num(c,"feature_flags"));
 traits_.set_visual_min_temperature(num(c,"visual_min_temperature"));
 traits_.set_visual_max_temperature(num(c,"visual_max_temperature"));
 traits_.set_visual_target_temperature_step(num(c,"visual_target_temperature_step"));
 traits_.set_visual_current_temperature_step(num(c,"visual_current_temperature_step"));
 traits_.set_visual_min_humidity(num(c,"visual_min_humidity"));
 traits_.set_visual_max_humidity(num(c,"visual_max_humidity"));
 const cJSON *v_modes; cJSON_ArrayForEach(v_modes,field(c,"modes")) traits_.add_supported_mode(static_cast<climate::ClimateMode>(int(v_modes->valuedouble)));
 const cJSON *v_fan_modes; cJSON_ArrayForEach(v_fan_modes,field(c,"fan_modes")) traits_.add_supported_fan_mode(static_cast<climate::ClimateFanMode>(int(v_fan_modes->valuedouble)));
 const cJSON *v_swing_modes; cJSON_ArrayForEach(v_swing_modes,field(c,"swing_modes")) traits_.add_supported_swing_mode(static_cast<climate::ClimateSwingMode>(int(v_swing_modes->valuedouble)));
 const cJSON *v_presets; cJSON_ArrayForEach(v_presets,field(c,"presets")) traits_.add_supported_preset(static_cast<climate::ClimatePreset>(int(v_presets->valuedouble)));
 custom_fans_=read_strings(c,"custom_fan_modes"); custom_presets_=read_strings(c,"custom_presets"); this->set_supported_custom_fan_modes(pointers(custom_fans_)); this->set_supported_custom_presets(pointers(custom_presets_)); traits_.set_temperature_unit(static_cast<TemperatureUnit>(int(num(c,"temperature_unit"))));
 this->apply=[this](const cJSON *v) {
 if(field(v,"current_temperature")) this->current_temperature=num(v,"current_temperature");
 if(field(v,"current_humidity")) this->current_humidity=num(v,"current_humidity");
 if(field(v,"target_humidity")) this->target_humidity=num(v,"target_humidity");
 if(field(v,"target_temperature")) this->target_temperature=num(v,"target_temperature");
 if(field(v,"target_temperature_low")) this->target_temperature_low=num(v,"target_temperature_low");
 if(field(v,"target_temperature_high")) this->target_temperature_high=num(v,"target_temperature_high");
 if(field(v,"mode")) this->mode=static_cast<climate::ClimateMode>(int(num(v,"mode")));
 if(field(v,"action")) this->action=static_cast<climate::ClimateAction>(int(num(v,"action")));
 if(field(v,"swing_mode")) this->swing_mode=static_cast<climate::ClimateSwingMode>(int(num(v,"swing_mode")));
 this->fan_mode.reset(); this->preset.reset(); this->clear_custom_fan_mode_(); this->clear_custom_preset_(); if(field(v,"fan_mode")) this->set_fan_mode_(static_cast<climate::ClimateFanMode>(int(num(v,"fan_mode")))); if(field(v,"preset")) this->set_preset_(static_cast<climate::ClimatePreset>(int(num(v,"preset")))); if(field(v,"custom_fan_mode")) this->set_custom_fan_mode_(string(v,"custom_fan_mode").c_str()); if(field(v,"custom_preset")) this->set_custom_preset_(string(v,"custom_preset").c_str()); this->publish_state();
 };
 }
 climate::ClimateTraits traits() override { return traits_; }
 protected:
 climate::ClimateTraits traits_;
 std::vector<std::string> custom_fans_,custom_presets_;
 void control(const climate::ClimateCall &call) override { auto *j=cJSON_CreateObject();
 optional_n(j,"target_temperature",call.get_target_temperature());
 optional_n(j,"target_temperature_low",call.get_target_temperature_low());
 optional_n(j,"target_temperature_high",call.get_target_temperature_high());
 optional_n(j,"target_humidity",call.get_target_humidity());
 optional_n(j,"mode",call.get_mode());
 optional_n(j,"fan_mode",call.get_fan_mode());
 optional_n(j,"swing_mode",call.get_swing_mode());
 optional_n(j,"preset",call.get_preset());
 if (call.has_custom_fan_mode()) {
   put_string(j, "custom_fan_mode", call.get_custom_fan_mode().c_str());
 }
 if (call.has_custom_preset()) {
   put_string(j, "custom_preset", call.get_custom_preset().c_str());
 }
 this->sender_(j); cJSON_Delete(j); }
};
#endif
#ifdef USE_LIGHT
class ProxyOutput : public light::LightOutput {
 public: light::LightTraits traits;
 light::LightTraits get_traits() override {return traits;}
 void write_state(light::LightState *) override {}
};
class ProxyEffect : public light::LightEffect {
 public: explicit ProxyEffect(const char *name):LightEffect(name) {} void apply() override {}
};
class RemoteLight : public light::LightState, public Proxy {
 public:
 RemoteLight(const cJSON *c,Sender sender):LightState(&output_),sender_(std::move(sender)) {
   this->entity=this;
   light::ColorModeMask modes; const cJSON *v;
   cJSON_ArrayForEach(v,field(c,"color_modes")) modes.insert(static_cast<light::ColorMode>(int(v->valuedouble)));
   output_.traits.set_supported_color_modes(modes);
   output_.traits.set_min_mireds(num(c,"min_mireds")); output_.traits.set_max_mireds(num(c,"max_mireds"));
   effect_names_=read_strings(c,"effects");
   this->effects_.init(effect_names_.size());
   for(auto &name:effect_names_) this->effects_.push_back(new ProxyEffect(name.c_str()));
   this->apply=[this](const cJSON *j) {
     remote_values.set_state(yes(j,"state")); remote_values.set_color_mode(static_cast<light::ColorMode>(int(num(j,"color_mode"))));
     if(field(j,"brightness")) remote_values.set_brightness(num(j,"brightness"));
     if(field(j,"color_brightness")) remote_values.set_color_brightness(num(j,"color_brightness"));
     if(field(j,"red")) remote_values.set_red(num(j,"red"));
     if(field(j,"green")) remote_values.set_green(num(j,"green"));
     if(field(j,"blue")) remote_values.set_blue(num(j,"blue"));
     if(field(j,"white")) remote_values.set_white(num(j,"white"));
     if(field(j,"color_temperature")) remote_values.set_color_temperature(num(j,"color_temperature"));
     if(field(j,"cold_white")) remote_values.set_cold_white(num(j,"cold_white"));
     if(field(j,"warm_white")) remote_values.set_warm_white(num(j,"warm_white"));
     current_values=remote_values; active_effect_index_=get_effect_index(string(j,"effect","None")); publish_state();
   };
 }
 void set_device(Device *d) override {this->set_device_(d);}
 void forward(const cJSON *j) {sender_(j);}
 protected: ProxyOutput output_; Sender sender_; std::vector<std::string> effect_names_;
};
static std::vector<RemoteLight *> remote_lights;
#endif
bool route_light(uint32_t key,uint32_t device,const cJSON *j) {
#ifdef USE_LIGHT
 for(auto *s:remote_lights) if(s->get_object_id_hash()==key && s->get_device_id()==device) {s->forward(j); return true;}
#endif
 return false;
}
std::unique_ptr<Proxy> make_proxy(const std::string &kind,const cJSON *c,Sender sender) {
#ifdef USE_SWITCH
 if(kind=="switch") return std::make_unique<RemoteSwitch>(c,std::move(sender));
#endif
#ifdef USE_NUMBER
 if(kind=="number") return std::make_unique<RemoteNumber>(c,std::move(sender));
#endif
#ifdef USE_SELECT
 if(kind=="select") return std::make_unique<RemoteSelect>(c,std::move(sender));
#endif
#ifdef USE_BUTTON
 if(kind=="button") return std::make_unique<RemoteButton>(c,std::move(sender));
#endif
#ifdef USE_TEXT
 if(kind=="input_text") return std::make_unique<RemoteText>(c,std::move(sender));
#endif
#ifdef USE_FAN
 if(kind=="fan") { auto p=std::make_unique<RemoteFan>(c,std::move(sender)); return p; }
#endif
#ifdef USE_COVER
 if(kind=="cover") { auto p=std::make_unique<RemoteCover>(c,std::move(sender)); return p; }
#endif
#ifdef USE_VALVE
 if(kind=="valve") { auto p=std::make_unique<RemoteValve>(c,std::move(sender)); return p; }
#endif
#ifdef USE_LOCK
 if(kind=="lock") { auto p=std::make_unique<RemoteLock>(c,std::move(sender)); return p; }
#endif
#ifdef USE_LIGHT
 if(kind=="light") { auto p=std::make_unique<RemoteLight>(c,std::move(sender)); remote_lights.push_back(p.get()); return p; }
#endif
#ifdef USE_CLIMATE
 if(kind=="climate") { auto p=std::make_unique<RemoteClimate>(c,std::move(sender)); return p; }
#endif
 return nullptr;
}
#else
bool route_light(uint32_t,uint32_t,const cJSON *) {return false;}
std::unique_ptr<Proxy> make_proxy(const std::string &,const cJSON *,Sender) {return nullptr;}
#endif
} // namespace esphome::thread_entity_bridge
