#pragma once
#include "esphome/core/defines.h"
#if defined(USE_OPENTHREAD) && defined(USE_BINARY_SENSOR)
#include "esphome/core/component.h"
#include "esphome/components/binary_sensor/binary_sensor.h"
#include "esphome/components/openthread/openthread.h"

namespace esphome::openthread_info {
class ThreadAttachOpenThreadInfo final : public binary_sensor::BinarySensor, public Component {
 public:
  bool is_thread_connected() const {
    auto *thread = openthread::global_openthread_component;
    return thread != nullptr && thread->is_connected();
  }
  float get_setup_priority() const override { return setup_priority::LATE; }
  void setup() override;
  void loop() override;
  void dump_config() override;

 protected:
  bool last_attached_{false};
};
}  // namespace esphome::openthread_info
#endif
