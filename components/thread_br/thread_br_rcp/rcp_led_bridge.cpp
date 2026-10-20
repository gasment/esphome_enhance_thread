#include "rcp_led_bridge.h"

#include <cstring>
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

static portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
static thread_br_rcp_led_state_t state{};

extern "C" void thread_br_rcp_led_report(const uint8_t *data, uint16_t length) {
  if (length != 9 || std::memcmp(data, "H2LE", 4) != 0 || data[4] != 2 ||
      (data[5] & ~3u) || data[6] > 1) return;
  const int64_t now = esp_timer_get_time();
  portENTER_CRITICAL(&lock);
  state = {true, data[5], data[6] != 0, static_cast<uint16_t>(data[7] | (data[8] << 8)), now};
  portEXIT_CRITICAL(&lock);
}

extern "C" thread_br_rcp_led_state_t thread_br_rcp_led_state(void) {
  portENTER_CRITICAL(&lock);
  auto copy = state;
  portEXIT_CRITICAL(&lock);
  return copy;
}

extern "C" void thread_br_rcp_led_invalidate(void) {
  portENTER_CRITICAL(&lock);
  state.valid = false;
  portEXIT_CRITICAL(&lock);
}
