
#include "openthread_info_text_sensor.h"
#ifdef USE_OPENTHREAD
#include "esphome/core/log.h"
#include <algorithm>
#if OPENTHREAD_FTD
#include <openthread/thread_ftd.h>
#endif

namespace esphome::openthread_info {

static const char *const TAG = "openthread_info";

void IPAddressOpenThreadInfo::dump_config() { LOG_TEXT_SENSOR("", "IPAddress", this); }
void AddressOpenThreadInfo::dump_config() { LOG_TEXT_SENSOR("", "Thread Internal Address", this); }
void BestRouterLinkQualityOpenThreadInfo::update_instance(otInstance *instance) {
  const char *rating = "unknown";
#if OPENTHREAD_FTD
  const auto role = otThreadGetDeviceRole(instance);
  if (role == OT_DEVICE_ROLE_ROUTER || role == OT_DEVICE_ROLE_LEADER) {
    uint8_t best = 0;
    const uint16_t own_rloc = otThreadGetRloc16(instance);
    const uint8_t max_id = otThreadGetMaxRouterId(instance);
    for (uint16_t id = 0; id <= max_id; id++) {
      otRouterInfo info{};
      if (otThreadGetRouterInfo(instance, id, &info) != OT_ERROR_NONE || !info.mAllocated ||
          !info.mLinkEstablished || info.mRloc16 == own_rloc) {
        continue;
      }
      best = std::max(best, std::min(info.mLinkQualityIn, info.mLinkQualityOut));
      if (best == 3) break;
    }
    static const char *const RATINGS[] = {"worst", "normal", "good", "best"};
    rating = RATINGS[best];
  }
#else
  (void) instance;
#endif
  // Clear the old Router rating after demotion or detachment; publish only on change.
  if (!this->has_state() || this->last_quality_ != rating) {
    this->last_quality_ = rating;
    this->publish_state(rating);
  }
}

void BestRouterLinkQualityOpenThreadInfo::dump_config() {
  LOG_TEXT_SENSOR("", "Best Router Link Quality", this);
}

void RoleOpenThreadInfo::dump_config() { LOG_TEXT_SENSOR("", "Role", this); }
void ChannelOpenThreadInfo::dump_config() { LOG_TEXT_SENSOR("", "Channel", this); }
void Rloc16OpenThreadInfo::dump_config() { LOG_TEXT_SENSOR("", "Rloc16", this); }
void ExtAddrOpenThreadInfo::dump_config() { LOG_TEXT_SENSOR("", "ExtAddr", this); }
void Eui64OpenThreadInfo::dump_config() { LOG_TEXT_SENSOR("", "Eui64", this); }
void NetworkNameOpenThreadInfo::dump_config() { LOG_TEXT_SENSOR("", "Network Name", this); }
void NetworkKeyOpenThreadInfo::dump_config() { LOG_TEXT_SENSOR("", "Network Key", this); }
void PanIdOpenThreadInfo::dump_config() { LOG_TEXT_SENSOR("", "PAN ID", this); }
void ExtPanIdOpenThreadInfo::dump_config() { LOG_TEXT_SENSOR("", "Extended PAN ID", this); }

}  // namespace esphome::openthread_info
#endif
