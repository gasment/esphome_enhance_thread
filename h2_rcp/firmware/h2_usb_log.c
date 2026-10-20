#include <stdarg.h>
#include <stdio.h>

#include "h2_usb_log.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_err.h"
#include "esp_log.h"
#include "openthread/instance.h"
#include "openthread/platform/logging.h"

static int usb_log_vprintf(const char *format, va_list args)
{
    char buffer[768];
    int length = vsnprintf(buffer, sizeof(buffer), format, args);
    if (length <= 0) return length;
    size_t size = (size_t) length < sizeof(buffer) ? (size_t) length : sizeof(buffer) - 1;
    // No PC/terminal must never stall the OpenThread worker. Logs can be
    // dropped if the USB TX queue is full; the periodic status will recur.
    return usb_serial_jtag_write_bytes(buffer, size, 0);
}

void h2_usb_log_init(void)
{
    if (!usb_serial_jtag_is_driver_installed()) {
        usb_serial_jtag_driver_config_t config = {
            .tx_buffer_size = 4096,
            .rx_buffer_size = 256,
        };
        ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&config));
    }
    usb_serial_jtag_vfs_use_driver();
    // ESP_LOG goes straight to the USB driver instead of depending on the
    // stdout FILE state after early writes without a connected USB host.
    esp_log_set_vprintf(usb_log_vprintf);
    ESP_LOGI("h2_usb", "USB Serial/JTAG application logger ready (INFO)");
}

// RCP normally forwards OpenThread logs over Spinel. Send them through the
// ESP-IDF console instead; the build fixes that console to USB Serial/JTAG.
static void log_line(otLogLevel level, const char *line)
{
    switch (level) {
    case OT_LOG_LEVEL_NONE: break;
    case OT_LOG_LEVEL_CRIT: ESP_LOGE("h2_ot", "%s", line); break;
    case OT_LOG_LEVEL_WARN: ESP_LOGW("h2_ot", "%s", line); break;
    case OT_LOG_LEVEL_NOTE:
    case OT_LOG_LEVEL_INFO: ESP_LOGI("h2_ot", "%s", line); break;
    default: ESP_LOGD("h2_ot", "%s", line); break;
    }
}

void __wrap_otPlatLogOutput(otInstance *instance, otLogLevel level, const char *line)
{
    (void) instance;
    log_line(level, line);
}

void __wrap_otPlatLog(otLogLevel level, otLogRegion region, const char *format, ...)
{
    (void) region;
    char line[512];
    va_list args;
    va_start(args, format);
    vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    log_line(level, line);
}
