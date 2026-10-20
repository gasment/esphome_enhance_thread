#include "esphome/core/defines.h"
#if defined(USE_ESP32) && defined(USE_OPENTHREAD)
namespace esphome::openthread {
// A setup reference extracts this translation unit from libsrc before the
// linker discovers wrapped references from the later lwIP archive.
void register_dns_debug() {}
}  // namespace esphome::openthread
#include "sdkconfig.h"
#if CONFIG_OPENTHREAD_DNS64_CLIENT
#include "esphome/core/hal.h"
#include "esphome/core/log.h"
#include "esp_netif.h"
#include "esp_openthread_netif_glue.h"
#include "lwip/dns.h"
#include "lwip/ip_addr.h"

// Observe the existing IDF resolver rather than replacing its DNS64 behavior.
extern "C" int __real_lwip_hook_dns_external_resolve(const char *name, ip_addr_t *addr,
                                                   dns_found_callback found, void *callback_arg,
                                                   u8_t addrtype, err_t *err);

#if ESPHOME_LOG_LEVEL >= ESPHOME_LOG_LEVEL_DEBUG
namespace esphome::openthread {
namespace {
static const char *const DNS_TAG = "openthread.dns";
struct DnsTrace {
  bool used{false};
  bool literal{false};
  uint32_t id{0}, started{0};
  dns_found_callback callback{nullptr};
  void *argument{nullptr};
};
// The lwIP DNS hook and completions execute in the TCP/IP core's serialized context,
// just like the IDF DNS64 resolver table. No allocation or extra DNS requests.
DnsTrace traces[DNS_TABLE_SIZE];
uint32_t next_id{0};

void log_result(const char *name, const ip_addr_t *address, const DnsTrace &trace, const char *mode) {
  const char *safe_name = name != nullptr ? name : "(unknown)";
  const unsigned elapsed = unsigned(esphome::millis() - trace.started);
  if (address == nullptr) {
    ESP_LOGD(DNS_TAG, "Resolve #%u '%s' failed: no result (timeout or negative response), %ums",
             unsigned(trace.id), safe_name, elapsed);
    return;
  }
  char result[IPADDR_STRLEN_MAX]{};
  ipaddr_ntoa_r(address, result, sizeof(result));
  if (IP_IS_V6(address)) {
    // IDF's DNS64 hook resolves A records, then puts the IPv4 address in the
    // final 32 bits of its synthesized IPv6 result.
    ip4_addr_t ipv4{};
    ipv4.addr = ip_2_ip6(address)->addr[3];
    char original[IP4ADDR_STRLEN_MAX]{};
    ip4addr_ntoa_r(&ipv4, original, sizeof(original));
    ESP_LOGD(DNS_TAG, "%s #%u '%s' -> IPv4 %s -> NAT64 %s (%s, %ums)",
             trace.literal ? "Literal" : "Resolve", unsigned(trace.id), safe_name, original, result, mode, elapsed);
  } else {
    ESP_LOGD(DNS_TAG, "%s #%u '%s' -> %s (no NAT64 synthesis, %s, %ums)",
             trace.literal ? "Literal" : "Resolve", unsigned(trace.id), safe_name, result, mode, elapsed);
  }
}

void dns_trace_found(const char *name, const ip_addr_t *address, void *argument) {
  auto *entry = static_cast<DnsTrace *>(argument);
  // Release before user callbacks so reentrant lookups cannot overwrite this record.
  const DnsTrace completed = *entry;
  entry->used = false;
  log_result(name, address, completed, "async");
  if (completed.callback != nullptr) completed.callback(name, address, completed.argument);
}
}  // namespace
}  // namespace esphome::openthread
#endif

extern "C" int __wrap_lwip_hook_dns_external_resolve(const char *name, ip_addr_t *addr,
                                                   dns_found_callback found, void *callback_arg,
                                                   u8_t addrtype, err_t *err) {
#if ESPHOME_LOG_LEVEL >= ESPHOME_LOG_LEVEL_DEBUG
  using namespace esphome::openthread;
  // IDF recursively requests IPv4 internally; only trace the outer Thread lookup.
  // Leave unrelated network interfaces and callback-less requests unchanged.
  if (addrtype == LWIP_DNS_ADDRTYPE_IPV4 ||
      esp_netif_get_default_netif() != esp_openthread_get_netif() ||
      name == nullptr || addr == nullptr || err == nullptr || found == nullptr) {
    return __real_lwip_hook_dns_external_resolve(name, addr, found, callback_arg, addrtype, err);
  }
  DnsTrace *entry = nullptr;
  for (auto &candidate : traces) {
    if (!candidate.used) { entry = &candidate; break; }
  }
  if (entry == nullptr) {
    ESP_LOGD(DNS_TAG, "Resolve '%s': trace slots full; forwarding without tracing", name);
    return __real_lwip_hook_dns_external_resolve(name, addr, found, callback_arg, addrtype, err);
  }
  ip_addr_t literal{};
  const bool is_literal = ipaddr_aton(name, &literal) != 0;
  *entry = {true, is_literal, ++next_id, esphome::millis(), found, callback_arg};
  const DnsTrace request = *entry;
  if (!is_literal) {
    char server[IPADDR_STRLEN_MAX]{};
    const auto *primary = dns_getserver(0);
    if (primary != nullptr) ipaddr_ntoa_r(primary, server, sizeof(server));
    ESP_LOGD(DNS_TAG, "Resolve #%u '%s': configured DNS IPv4=%s, primary=%s",
             unsigned(request.id), name, CONFIG_OPENTHREAD_DNS_SERVER_ADDR, server);
  }
  const int handled = __real_lwip_hook_dns_external_resolve(name, addr, dns_trace_found, entry, addrtype, err);
  if (!handled || *err != ERR_INPROGRESS) {
    entry->used = false;
    if (handled && *err == ERR_OK) {
      log_result(name, addr, request, is_literal ? "local conversion; no DNS query" : "immediate/cache");
    } else if (handled) {
      ESP_LOGD(DNS_TAG, "Resolve #%u '%s' failed: lwIP error=%d",
               unsigned(request.id), name, int(*err));
    }
  }
  return handled;
#else
  return __real_lwip_hook_dns_external_resolve(name, addr, found, callback_arg, addrtype, err);
#endif
}
#endif  // CONFIG_OPENTHREAD_DNS64_CLIENT
#endif  // USE_ESP32 && USE_OPENTHREAD
