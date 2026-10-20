#include "h2_leds.h"

#include <atomic>
#include "esp_openthread_task_queue.h"
#include "esp_timer.h"
#include "ncp_base.hpp"

static std::atomic<bool> queued{false};

static void report_on_openthread_task(void *)
{
    uint8_t data[9];
    uint32_t generation;
    auto *ncp = ot::Ncp::NcpBase::GetNcpInstance();
    if (ncp && h2_leds_report_snapshot(data, &generation) &&
        ncp->StreamWrite(H2_LED_STATUS_PROPERTY, data, sizeof(data)) == OT_ERROR_NONE) {
        h2_leds_report_sent(generation);
    }
    queued = false;
}

extern "C" void h2_led_report_poll(void)
{
    // Called only by the LED task. Retry queue/buffer failures at most 5Hz.
    static int64_t last_attempt;
    int64_t now = esp_timer_get_time();
    if (now - last_attempt < 200000) return;
    uint8_t data[9];
    uint32_t generation;
    if (!h2_leds_report_snapshot(data, &generation) || queued.exchange(true)) return;
    last_attempt = now;
    if (esp_openthread_task_queue_post(report_on_openthread_task, nullptr) != ESP_OK) queued = false;
}
