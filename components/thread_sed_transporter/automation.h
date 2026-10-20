#pragma once

#include "esphome/core/automation.h"
#include "thread_sed_transporter.h"

namespace esphome::thread_sed_transporter {

template<typename... Ts> class ReportAction : public Action<Ts...> {
 public:
  explicit ReportAction(ThreadSedTransporter *parent) : parent_(parent) {}
  void set_ack_timeout(uint32_t value) { this->ack_timeout_ = value; }
  void set_ack_resend(uint8_t value) { this->ack_resend_ = value; }
  void play(const Ts &...x) override { this->parent_->report(this->ack_timeout_, this->ack_resend_); }

 protected:
  ThreadSedTransporter *parent_;
  uint32_t ack_timeout_{0};
  uint8_t ack_resend_{0};
};

class ReportCompleteTrigger : public Trigger<bool, uint32_t, uint32_t, uint32_t, uint32_t> {
 public:
  explicit ReportCompleteTrigger(ThreadSedTransporter *parent) {
    parent->add_on_complete_callback([this](bool success, uint32_t sent, uint32_t unchanged,
                                          uint32_t invalid, uint32_t errors) {
      this->trigger(success, sent, unchanged, invalid, errors);
    });
  }
};

class ReportSuccessTrigger : public Trigger<uint32_t> {
 public:
  explicit ReportSuccessTrigger(ThreadSedTransporter *parent) {
    parent->add_on_report_success_callback([this](uint32_t report_id) { this->trigger(report_id); });
  }
};

class ReportFailTrigger : public Trigger<uint32_t> {
 public:
  explicit ReportFailTrigger(ThreadSedTransporter *parent) {
    parent->add_on_report_fail_callback([this](uint32_t report_id) { this->trigger(report_id); });
  }
};

template<typename... Ts> class DiscoveryAction : public Action<Ts...> {
 public:
  explicit DiscoveryAction(ThreadSedTransporter *parent) : parent_(parent) {}
  void play(const Ts &...x) override { parent_->discovery(); }
 protected:
  ThreadSedTransporter *parent_;
};

}  // namespace esphome::thread_sed_transporter
