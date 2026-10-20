#pragma once

#include <limits>
#include <cstdint>

namespace esphome::thread_sed_transporter {

template<typename T> constexpr bool changed_value(const T &current, const T &baseline, bool has_baseline) {
  return !has_baseline || current != baseline;
}

enum class ReportOutcome { CHANGED, UNCHANGED, INVALID, ERROR };

struct ReportSummary {
  uint32_t sent{0};
  uint32_t unchanged{0};
  uint32_t invalid{0};
  uint32_t errors{0};

  constexpr void add(ReportOutcome outcome) {
    switch (outcome) {
      case ReportOutcome::CHANGED: break;
      case ReportOutcome::UNCHANGED: ++this->unchanged; break;
      case ReportOutcome::INVALID: ++this->invalid; break;
      case ReportOutcome::ERROR: ++this->errors; break;
    }
  }
  constexpr bool success() const { return this->invalid == 0 && this->errors == 0; }
};

constexpr bool finite_sample(float value) {
  constexpr float max = std::numeric_limits<float>::max();
  return value == value && value <= max && value >= -max;
}

constexpr bool should_report(float current, float baseline, float delta, bool has_baseline) {
  if (!finite_sample(current))
    return false;
  if (!has_baseline || !finite_sample(baseline))
    return true;
  // delta=0 means any actual change, rather than unconditional reporting.
  const double difference = static_cast<double>(current) - baseline;
  const double magnitude = difference < 0 ? -difference : difference;
  return current != baseline && magnitude >= delta;
}

}  // namespace esphome::thread_sed_transporter
