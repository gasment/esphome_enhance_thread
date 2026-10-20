#include "h2_leds.h"
#include "h2_led_config.h"

#include <string.h>
#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "nvs.h"
#include "openthread/error.h"

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static int64_t s_last_activity;
static int64_t s_last_heartbeat;
static bool s_seen_activity;
static bool s_host_healthy;
static bool s_connected;
static bool s_reporting;
static uint16_t s_ack_sequence;
static uint32_t s_generation = 1;
static uint32_t s_reported_generation;
static int64_t s_last_report;
static uint8_t s_enabled = (H2_LED_ACTIVITY_ENABLED_ON_BOOT ? 1 : 0) |
                           (H2_LED_CONNECTION_ENABLED_ON_BOOT ? 2 : 0);
static nvs_handle_t s_led_nvs;
static bool s_storage_ready;
static bool s_enabled_saved;
typedef struct {
    uint8_t mask;
    uint8_t values;
    uint16_t sequence;
} led_setting_t;
static QueueHandle_t s_settings;

static void load_enabled(void)
{
    // app_main initializes NVS before calling h2_leds_init(). Never erase shared NVS.
    esp_err_t err = nvs_open("h2_leds", NVS_READWRITE, &s_led_nvs);
    if (err != ESP_OK) {
        ESP_LOGE("h2_leds", "LED NVS open failed: %s", esp_err_to_name(err));
        return;
    }
    s_storage_ready = true;
    uint8_t enabled;
    err = nvs_get_u8(s_led_nvs, "led_enabled", &enabled);
    if (err == ESP_OK && (enabled & ~3u) != 0) err = ESP_ERR_INVALID_ARG;
    if (err == ESP_OK) {
        s_enabled = enabled;
        s_enabled_saved = true;
        ESP_LOGI("h2_leds", "Restored led_enabled=%u from H2 NVS", (unsigned) enabled);
    } else if (err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW("h2_leds", "Invalid/unreadable LED NVS value; using boot defaults (%s)", esp_err_to_name(err));
    }
}

static void apply_setting(const led_setting_t *setting)
{
    portENTER_CRITICAL(&s_lock);
    const uint8_t previous = s_enabled;
    portEXIT_CRITICAL(&s_lock);
    const uint8_t next = (previous & ~setting->mask) | (setting->values & setting->mask);
    // Only the LED task accesses NVS. Persist before updating state/sequence so a
    // reported SET acknowledgement also confirms durability. Retries do not rewrite.
    esp_err_t err = s_storage_ready ? ESP_OK : ESP_ERR_INVALID_STATE;
    if (err == ESP_OK && (!s_enabled_saved || next != previous)) {
        err = nvs_set_u8(s_led_nvs, "led_enabled", next);
        if (err == ESP_OK) err = nvs_commit(s_led_nvs);
    }
    if (err != ESP_OK) {
        ESP_LOGE("h2_leds", "LED setting not applied: NVS save failed (%s)", esp_err_to_name(err));
        return;
    }
    s_enabled_saved = true;
    portENTER_CRITICAL(&s_lock);
    s_enabled = next;
    s_ack_sequence = setting->sequence;
    s_generation++;
    portEXIT_CRITICAL(&s_lock);
    ESP_LOGI("h2_leds", "LED setting persisted: seq=%u led_enabled=%u",
             (unsigned) setting->sequence, (unsigned) next);
}


static void activity(void)
{
    int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&s_lock);
    s_last_activity = now;
    s_seen_activity = true;
    portEXIT_CRITICAL(&s_lock);
}

void __real_otPlatUartReceived(const uint8_t *buffer, uint16_t length);
otError __real_otPlatUartSend(const uint8_t *buffer, uint16_t length);

void __wrap_otPlatUartReceived(const uint8_t *buffer, uint16_t length)
{
    if (length) activity();
    __real_otPlatUartReceived(buffer, length);
}

otError __wrap_otPlatUartSend(const uint8_t *buffer, uint16_t length)
{
    otError error = __real_otPlatUartSend(buffer, length);
    if (length && error == OT_ERROR_NONE) activity();
    return error;
}

bool h2_leds_control(const uint8_t *data, uint16_t length)
{
    // H2LE, v2, operation, update mask, enable values, health, sequence LE16.
    if (length != 11 || memcmp(data, "H2LE", 4) != 0 || data[4] != 2 ||
        data[5] > 2 || (data[6] & ~3u) || (data[7] & ~3u) || data[8] > 1 ||
        (data[5] != 1 && (data[6] || data[7] || data[9] || data[10])) ||
        (data[5] == 1 && (!data[6] || !(data[9] || data[10])))) {
        ESP_LOGW("h2_leds", "Rejected invalid H2LE control payload");
        return false;
    }
    if (data[5] == 1) {
        const led_setting_t setting = {
            .mask = data[6], .values = data[7],
            .sequence = data[9] | ((uint16_t) data[10] << 8),
        };
        // Never block the Spinel/OT receive task or perform Flash IO under s_lock.
        if (!s_settings || xQueueSend(s_settings, &setting, 0) != pdTRUE) {
            ESP_LOGW("h2_leds", "LED setting queue unavailable/full");
            return false;
        }
        return true;
    }
    int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&s_lock);
    if (data[5] == 0) {
        s_host_healthy = data[8] != 0;
        s_last_heartbeat = now;
    } else {
        s_generation++;  // QUERY requests a fresh status report, without changing LED settings.
    }
    portEXIT_CRITICAL(&s_lock);
    return true;
}

void h2_leds_start_reporting(void)
{
    portENTER_CRITICAL(&s_lock);
    s_reporting = true;
    portEXIT_CRITICAL(&s_lock);
}

bool h2_leds_report_snapshot(uint8_t data[9], uint32_t *generation)
{
    int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&s_lock);
    bool due = s_reporting && (s_generation != s_reported_generation || now - s_last_report >= 30000000LL);
    if (due) {
        memcpy(data, "H2LE", 4);
        data[4] = 2;
        data[5] = s_enabled;
        data[6] = s_connected;
        data[7] = s_ack_sequence & 0xff;
        data[8] = s_ack_sequence >> 8;
        *generation = s_generation;
    }
    portEXIT_CRITICAL(&s_lock);
    return due;
}

void h2_leds_report_sent(uint32_t generation)
{
    int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&s_lock);
    s_reported_generation = generation;
    s_last_report = now;
    portEXIT_CRITICAL(&s_lock);
}

static void led_task(void *arg)
{
    (void) arg;
    bool flashing = false;
    int64_t activity_start = 0;
    int64_t last_status = 0;
    for (;;) {
        led_setting_t setting;
        if (xQueueReceive(s_settings, &setting, 0) == pdTRUE) apply_setting(&setting);
        int64_t now = esp_timer_get_time();
        portENTER_CRITICAL(&s_lock);
        bool active = s_seen_activity && now - s_last_activity < H2_LED_ACTIVITY_HOLD_MS * 1000LL;
        bool connected = s_host_healthy && now - s_last_heartbeat < H2_LED_CONNECTION_TIMEOUT_MS * 1000LL;
        bool connection_changed = connected != s_connected;
        if (connection_changed) {
            s_connected = connected;
            s_generation++;
        }
        uint8_t enabled = s_enabled;
        portEXIT_CRITICAL(&s_lock);
        if (connection_changed) {
            if (connected) ESP_LOGI("h2_leds", "Host connected (healthy heartbeat)");
            else ESP_LOGW("h2_leds", "Host disconnected or heartbeat timed out; blue fault blink forced");
        }

        active = active && (enabled & 1);
        if (active && !flashing) activity_start = now;
        flashing = active;
        bool green = active && ((now - activity_start) / (H2_LED_ACTIVITY_BLINK_INTERVAL_MS * 1000LL)) % 2 == 0;
        // A disconnected RCP always reports its fault, even with both LEDs disabled.
        bool blue = connected ? (enabled & 2) != 0 :
                    (now / (H2_LED_CONNECTION_BLINK_INTERVAL_MS * 1000LL)) % 2 == 0;
        gpio_set_level(H2_LED_ACTIVITY_PIN, green ^ H2_LED_ACTIVITY_INVERTED);
        gpio_set_level(H2_LED_CONNECTION_PIN, blue ^ H2_LED_CONNECTION_INVERTED);
        if (now - last_status >= 10000000LL) {
            last_status = now;
            ESP_LOGI("h2_leds", "H2 alive: uptime=%lus host=%s green=%s blue=%s%s",
                     (unsigned long) (now / 1000000), connected ? "connected" : "disconnected",
                     (enabled & 1) ? "enabled" : "disabled", (enabled & 2) ? "enabled" : "disabled",
                     connected ? "" : " (fault blink forced)");
        }
        h2_led_report_poll();
        vTaskDelay(pdMS_TO_TICKS(10) ? pdMS_TO_TICKS(10) : 1);
    }
}

void h2_leds_init(void)
{
    load_enabled();
    s_settings = xQueueCreate(8, sizeof(led_setting_t));
    ESP_ERROR_CHECK(s_settings ? ESP_OK : ESP_ERR_NO_MEM);
    gpio_config_t config = {
        .pin_bit_mask = (1ULL << H2_LED_ACTIVITY_PIN) | (1ULL << H2_LED_CONNECTION_PIN),
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&config));
    gpio_set_level(H2_LED_ACTIVITY_PIN, H2_LED_ACTIVITY_INVERTED);
    gpio_set_level(H2_LED_CONNECTION_PIN, H2_LED_CONNECTION_INVERTED);
    ESP_LOGI("h2_leds", "H2 RCP: UART0/460800, H2LE/2, USB Serial/JTAG logs; awaiting Host");
    ESP_LOGI("h2_leds", "LED GPIOs: green=%d blue=%d; Host timeout=%dms; report interval=30s",
             H2_LED_ACTIVITY_PIN, H2_LED_CONNECTION_PIN, H2_LED_CONNECTION_TIMEOUT_MS);
    ESP_ERROR_CHECK(xTaskCreate(led_task, "h2_leds", 4096, NULL, 1, NULL) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
}
