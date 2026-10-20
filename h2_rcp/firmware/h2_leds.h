#pragma once

#include <stdbool.h>
#include <stdint.h>

#define H2_LED_SPINEL_COMMAND 15360
#define H2_LED_STATUS_PROPERTY 15360

#ifdef __cplusplus
extern "C" {
#endif

void h2_leds_init(void);
bool h2_leds_control(const uint8_t *data, uint16_t length);
void h2_leds_start_reporting(void);
void h2_led_report_poll(void);
bool h2_leds_report_snapshot(uint8_t data[9], uint32_t *generation);
void h2_leds_report_sent(uint32_t generation);

#ifdef __cplusplus
}
#endif
