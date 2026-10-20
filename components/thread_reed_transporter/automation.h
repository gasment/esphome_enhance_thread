#pragma once
#include "esphome/core/automation.h"
#include "thread_reed_transporter.h"
namespace esphome::thread_reed_transporter {
template<typename... Ts> class DiscoveryAction : public Action<Ts...> {
 public:
  explicit DiscoveryAction(ThreadReedTransporter *parent) : parent_(parent) {}
  void play(const Ts &...x) override { parent_->discovery(); }
 protected:
  ThreadReedTransporter *parent_;
};

}
