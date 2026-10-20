#pragma once

#include "esphome/core/automation.h"
#include "thread_gateway.h"

#ifdef USE_THREAD_GATEWAY
namespace esphome::thread_gateway {

class NodeSensorUpdateTrigger : public Trigger<float> {
 public:
  NodeSensorUpdateTrigger(ThreadGateway *parent, const std::string &node, const std::string &entity) {
    parent->add_sensor_report_callback(node, entity, [this](float x) { this->trigger(x); });
  }
};

class NodeBinaryUpdateTrigger : public Trigger<bool> {
 public:
  NodeBinaryUpdateTrigger(ThreadGateway *parent, const std::string &node, const std::string &entity) {
    parent->add_binary_report_callback(node, entity, [this](bool x) { this->trigger(x); });
  }
};

class NodeTextUpdateTrigger : public Trigger<std::string> {
 public:
  NodeTextUpdateTrigger(ThreadGateway *parent, const std::string &node, const std::string &entity) {
    parent->add_text_report_callback(node, entity, [this](std::string x) { this->trigger(std::move(x)); });
  }
};

}  // namespace esphome::thread_gateway
#endif
