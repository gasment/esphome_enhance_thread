#include "openthread_info_binary_sensor.h"
#if defined(USE_OPENTHREAD) && defined(USE_BINARY_SENSOR)
#include "esphome/core/log.h"

namespace esphome::openthread_info {
static const char *const TAG = "openthread_info.binary_sensor";
void ThreadAttachOpenThreadInfo::setup() {
  last_attached_ = is_thread_connected();
  publish_state(last_attached_);
}
void ThreadAttachOpenThreadInfo::loop() {
  // Read the component's cached attachment flag; do not call OT APIs or acquire its lock.
  const bool attached = is_thread_connected();
  if (attached != last_attached_) {
    last_attached_ = attached;
    publish_state(attached);
  }
}
void ThreadAttachOpenThreadInfo::dump_config() { LOG_BINARY_SENSOR("  ", "Thread attachment", this); }
}  // namespace esphome::openthread_info
#endif
