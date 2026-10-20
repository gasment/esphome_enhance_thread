#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool valid;
    uint8_t enabled;
    bool connected;
    uint16_t sequence;
    int64_t received_us;
} thread_br_rcp_led_state_t;

void thread_br_rcp_led_report(const uint8_t *data, uint16_t length);
thread_br_rcp_led_state_t thread_br_rcp_led_state(void);
void thread_br_rcp_led_invalidate(void);

#ifdef __cplusplus
}
#endif
