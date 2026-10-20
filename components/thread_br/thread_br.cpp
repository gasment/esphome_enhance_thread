#include "thread_br.h"

#include "esphome/core/log.h"
#include "esphome/components/wifi/wifi_component.h"
#ifdef USE_CAPTIVE_PORTAL
#include "esphome/components/captive_portal/captive_portal.h"
#endif
#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_coexist.h"
#include "esp_netif.h"
#include "esp_openthread.h"
#include "esp_openthread_border_router.h"
#include "esp_openthread_lock.h"
#include "esp_openthread_netif_glue.h"
#include "esp_openthread_spinel.h"
#include "esp_vfs_eventfd.h"
#include "thread_br_web.h"
#include "rcp_led_bridge.h"
#include "esp_timer.h"
#include "openthread/dataset.h"
#include "openthread/link.h"
#include "openthread/platform/radio.h"
#include "openthread/platform/infra_if.h"
#include "openthread/thread.h"
#include "openthread/thread_ftd.h"

namespace esphome::thread_br {
static const char *const TAG = "thread_br";
ThreadBorderRouter *ThreadBorderRouter::instance_ = nullptr;

void ThreadBorderRouter::setup() {
  instance_ = this;
  if (rcp_connectivity_) rcp_connectivity_->publish_state(false);
  if (thread_attached_) thread_attached_->publish_state(false);
  if (thread_role_) thread_role_->publish_state("starting");
  if (xTaskCreate(web_task_, "br_web", 4096, this, 3, nullptr) != pdPASS)
    state_ = State::FAILED;
}

void ThreadBorderRouter::loop() {
  const auto state = state_.load();
  if (state == State::WAITING) {
    // Spinel initialization can wait for hardware; never block ESPHome's loop.
    state_ = State::STARTING;
    if (xTaskCreate(start_task_, "br_start", 8192, this, 4, &startup_task_) != pdPASS)
      state_ = State::FAILED;
  } else if (state == State::FAILED && !failure_reported_) {
    failure_reported_ = true;
    ESP_LOGE(TAG, "BR initialization failed; ESPHome remains available for diagnostics");
    this->status_set_error(LOG_STR("BR initialization failed"));
    if (thread_role_) thread_role_->publish_state("failed");
  }
  const bool wifi_up = wifi::global_wifi_component->is_connected();
  want_backbone_ = wifi_up;
  bool serve = state == State::READY && wifi_up;
#ifdef USE_CAPTIVE_PORTAL
  // The portal only runs during fallback provisioning. Release our listener
  // while offline, and wait until the portal has stopped before binding again.
  if (captive_portal::global_captive_portal && captive_portal::global_captive_portal->is_active())
    serve = false;
#endif
  want_web_ = serve;
}

void ThreadBorderRouter::web_task_(void *arg) {
  auto *self = static_cast<ThreadBorderRouter *>(arg);
  bool started = false;
  uint32_t last_attempt = 0;
  while (self->state_ != State::STOPPING) {
    if (!self->want_web_ && started) {
      // httpd_stop waits for in-flight diagnostics. Keep it off the ESPHome
      // loop and off the Spinel health worker.
      thread_br_web_stop();
      started = false;
    } else if (self->want_web_ && !started && millis() - last_attempt >= 5000) {
      last_attempt = millis();
      started = thread_br_web_start(self->web_port_, self->auth_.c_str(), self->use_psram_) == ESP_OK;
      if (started) ESP_LOGI(TAG, "BR Web listening on port %u", self->web_port_);
      else ESP_LOGW(TAG, "BR Web could not bind port %u; retrying", self->web_port_);
    }
    vTaskDelay(pdMS_TO_TICKS(250));
  }
  vTaskDelete(nullptr);
}

void ThreadBorderRouter::start_task_(void *arg) {
  auto *self = static_cast<ThreadBorderRouter *>(arg);
  self->start_();
  uint32_t last_sample = millis() - 5000;
  while (self->state_ == State::READY) {
    if (millis() - last_sample >= 5000) {
      self->sample_();
      last_sample = millis();
    }
    self->service_backbone_();
    self->service_rcp_leds_();
    vTaskDelay(pdMS_TO_TICKS(100));
  }
  vTaskDelete(nullptr);
}

void ThreadBorderRouter::reset_rcp_() {
  thread_br_rcp_led_invalidate();
  // BOOT must be high before EN rises. Keep the ROM downloader accessible by
  // manual wiring, but never enter it during normal recovery.
  gpio_set_level(static_cast<gpio_num_t>(boot_), 1);
  gpio_set_level(static_cast<gpio_num_t>(reset_), 0);
  vTaskDelay(pdMS_TO_TICKS(20));
  gpio_set_level(static_cast<gpio_num_t>(reset_), 1);
  vTaskDelay(pdMS_TO_TICKS(100));
}

void ThreadBorderRouter::rcp_reset_failure_() {
  if (!instance_) return;
  instance_->rcp_fault_ = true;
  // IDF/Spinel owns retries and restores radio state after a successful reset.
  // Bound physical resets in one boot; a dead RCP must not trigger endless resets.
  if (instance_->reset_count_.fetch_add(1) < 3) instance_->reset_rcp_();
}

void ThreadBorderRouter::rcp_failure_() {
  // Runtime recovery is invoked by the Spinel stack, under its own lock.
  rcp_reset_failure_();
}

void ThreadBorderRouter::start_() {
  bool ok = initialize_();
  state_ = ok ? State::READY : State::FAILED;
}

bool ThreadBorderRouter::initialize_() {
  auto *backbone = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
  if (!backbone) return false;
  if (uart_is_driver_installed(static_cast<uart_port_t>(UART_PORT))) {
    ESP_LOGE(TAG, "UART%d is already owned by another component", UART_PORT);
    return false;
  }
  gpio_config_t pins{};
  pins.pin_bit_mask = (1ULL << reset_) | (1ULL << boot_);
  // Release-high open-drain outputs also permit the H2's manual BOOT/reset
  // controls to pull these lines low without fighting the S3 output driver.
  // Preload the released level before enabling outputs. A reset-default low
  // latch must not briefly assert EN/BOOT while gpio_config enables the pads.
  if (gpio_set_level(static_cast<gpio_num_t>(boot_), 1) != ESP_OK ||
      gpio_set_level(static_cast<gpio_num_t>(reset_), 1) != ESP_OK)
    return false;
  pins.mode = GPIO_MODE_OUTPUT_OD;
  pins.pull_up_en = GPIO_PULLUP_ENABLE;
  if (gpio_config(&pins) != ESP_OK) return false;
  reset_rcp_();

#if !CONFIG_ESP_COEX_EXTERNAL_COEXIST_ENABLE
#error "ESP Thread BR Router requires four-wire external coexistence"
#endif
  esp_external_coex_gpio_set_t coex_pins{};
  coex_pins.request = static_cast<gpio_num_t>(request_);
  coex_pins.priority = static_cast<gpio_num_t>(priority_);
  coex_pins.grant = static_cast<gpio_num_t>(grant_);
  coex_pins.tx_line = static_cast<gpio_num_t>(tx_line_);
  if (esp_external_coex_set_work_mode(EXTERNAL_COEX_LEADER_ROLE) != ESP_OK ||
      esp_enable_extern_coex_gpio_pin(EXTERN_COEX_WIRE_4, coex_pins) != ESP_OK)
    return false;
  esp_vfs_eventfd_config_t eventfd{};
  eventfd.max_fds = 3;  // Thread netif, task queue, border router (UART, no TREL).
  auto err = esp_vfs_eventfd_register(&eventfd);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "eventfd initialization: %s", esp_err_to_name(err));
    return false;
  }
  esp_openthread_register_rcp_failure_handler(rcp_failure_);
  esp_openthread_set_coprocessor_reset_failure_callback(rcp_reset_failure_);
  // Bind the ESPHome STA netif before the OpenThread platform initializes.
  esp_openthread_set_backbone_netif(backbone);
  auto &config = ot_config_;
  config.netif_config = ESP_NETIF_DEFAULT_OPENTHREAD();
  auto &radio = config.platform_config.radio_config;
  radio.radio_mode = RADIO_MODE_UART_RCP;
  radio.radio_uart_config.port = static_cast<uart_port_t>(UART_PORT);
  radio.radio_uart_config.rx_pin = static_cast<gpio_num_t>(rx_);
  radio.radio_uart_config.tx_pin = static_cast<gpio_num_t>(tx_);
  auto &uart = radio.radio_uart_config.uart_config;
  uart.baud_rate = BAUD_RATE;
  uart.data_bits = UART_DATA_8_BITS;
  uart.parity = UART_PARITY_DISABLE;
  uart.stop_bits = UART_STOP_BITS_1;
  uart.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
  uart.source_clk = UART_SCLK_DEFAULT;
  config.platform_config.host_config.host_connection_mode = HOST_CONNECTION_MODE_NONE;
  config.platform_config.port_config.storage_partition_name = "nvs";
  config.platform_config.port_config.netif_queue_size = 10;
  config.platform_config.port_config.task_queue_size = 10;
  // IDF starts and synchronizes its own OpenThread mainloop task here.
  err = esp_openthread_start(&config);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "OpenThread start: %s", esp_err_to_name(err));
    return false;
  }
  esp_openthread_lock_acquire(portMAX_DELAY);
  otOperationalDatasetTlvs dataset{};
  bool saved = !force_dataset_ &&
               otDatasetGetActiveTlvs(esp_openthread_get_instance(), &dataset) == OT_ERROR_NONE &&
               dataset.mLength > 0;
  ESP_LOGI(TAG, "Dataset source: %s", saved ? "NVS" : "firmware configuration");
  err = esp_openthread_auto_start(saved ? &dataset : nullptr);
  const char *version = otPlatRadioGetVersionString(esp_openthread_get_instance());
  version_ = version ? version : "unknown";
  esp_openthread_lock_release();
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Thread initialization: %s", esp_err_to_name(err));
    return false;
  }
  rcp_fault_ = false;
  healthy_ = true;
  ESP_LOGI(TAG, "Thread started independently of Wi-Fi; gateway UDP can start after attachment");
  return true;
}

void ThreadBorderRouter::service_backbone_() {
  const bool up = want_backbone_.load();
  if (up == backbone_up_) return;
  const uint32_t now = millis();
  if (up && backbone_retry_ && uint32_t(now - backbone_attempt_at_) < 10000) return;
  // Keep potentially blocking IDF operations off the ESPHome main loop.
  if (!esp_openthread_lock_acquire(pdMS_TO_TICKS(20))) return;
  if (!up) {
    if (border_router_started_) {
      auto *backbone = esp_openthread_get_backbone_netif();
      if (otPlatInfraIfStateChanged(esp_openthread_get_instance(),
                                  esp_netif_get_netif_impl_index(backbone), false) != OT_ERROR_NONE)
        ESP_LOGW(TAG, "Could not notify stack of backbone loss; BR will be rebuilt on reconnect");
    }
    backbone_up_ = false;
    backbone_retry_ = false;
    ESP_LOGI(TAG, "Wi-Fi backbone unavailable; Thread and gateway UDP remain active");
  } else {
    backbone_attempt_at_ = now;
    backbone_retry_ = true;
    esp_err_t err = ESP_OK;
    if (border_router_started_) {
      err = esp_openthread_border_router_deinit();
      if (err == ESP_OK) border_router_started_ = false;
    }
    if (err == ESP_OK) {
      err = esp_openthread_border_router_init();
      if (err == ESP_OK) {
        border_router_started_ = true;
        backbone_up_ = true;
        backbone_retry_ = false;
        ESP_LOGI(TAG, "Wi-Fi backbone ready; border routing started");
      }
    }
    if (err != ESP_OK)
      ESP_LOGW(TAG, "Border routing unavailable (%s); retry in 10s, Thread remains active", esp_err_to_name(err));
  }
  esp_openthread_lock_release();
}

void ThreadBorderRouter::sample_() {
  if (state_ != State::READY) return;
  // Only the worker probes Spinel: a missing RCP may make an individual
  // property request wait for several seconds, even after acquiring the lock.
  if (!esp_openthread_lock_acquire(pdMS_TO_TICKS(20))) {
    if (rcp_fault_) healthy_ = false;
    return;
  }
  auto *ot = esp_openthread_get_instance();
  const auto role = otThreadGetDeviceRole(ot);
  // A successful Spinel property round trip, not the Thread attachment state.
  int8_t power{};
  bool healthy = otPlatRadioGetTransmitPower(ot, &power) == OT_ERROR_NONE;
  // The heartbeat confirms a standard Spinel round trip, independently of
  // Thread attachment. A missing heartbeat makes the H2 indicate a fault.
  if (!send_rcp_led_control_(0, 0, 0, healthy, 0)) healthy = false;
  uint16_t neighbors = 0, children = 0;
  otNeighborInfoIterator iterator = OT_NEIGHBOR_INFO_ITERATOR_INIT;
  otNeighborInfo neighbor{};
  while (otThreadGetNextNeighborInfo(ot, &iterator, &neighbor) == OT_ERROR_NONE) neighbors++;
  otChildInfo child{};
  for (uint16_t i = 0; i < otThreadGetMaxAllowedChildren(ot); i++)
    if (otThreadGetChildInfoByIndex(ot, i, &child) == OT_ERROR_NONE && !child.mIsStateRestoring) children++;
  esp_openthread_lock_release();
  rcp_fault_ = !healthy;
  healthy_ = healthy;
  role_ = static_cast<unsigned>(role);
  neighbors_ = neighbors;
  children_ = children;
}

bool ThreadBorderRouter::send_rcp_led_control_(uint8_t operation, uint8_t update_mask,
                                               uint8_t values, bool healthy, uint16_t sequence) {
  // Spinel header: IID=0, TID=0. Vendor command 15360 is packed as 80 78.
  // H2 reports the applied sequence in a separate vendor property notification.
  const uint8_t frame[] = {0x80, 0x80, 0x78, 'H', '2', 'L', 'E', 2,
                           operation, update_mask, values, static_cast<uint8_t>(healthy),
                           static_cast<uint8_t>(sequence), static_cast<uint8_t>(sequence >> 8)};
  uint8_t encoded[2 * (sizeof(frame) + 2) + 2];
  size_t length = 0;
  encoded[length++] = 0x7e;
  auto append = [&](uint8_t byte) {
    if (byte == 0x7e || byte == 0x7d || byte == 0x11 || byte == 0x13 || byte == 0xf8) {
      encoded[length++] = 0x7d;
      byte ^= 0x20;
    }
    encoded[length++] = byte;
  };
  uint16_t crc = 0xffff;
  for (uint8_t byte : frame) {
    append(byte);
    crc ^= byte;
    for (unsigned bit = 0; bit < 8; bit++) crc = (crc >> 1) ^ ((crc & 1) ? 0x8408 : 0);
  }
  crc ^= 0xffff;
  append(static_cast<uint8_t>(crc));
  append(static_cast<uint8_t>(crc >> 8));
  encoded[length++] = 0x7e;
  // The lock serializes this complete HDLC frame with the radio driver's
  // frames. Do not register a second UART reader or reconfigure the driver.
  return uart_write_bytes(static_cast<uart_port_t>(UART_PORT), encoded, length) == static_cast<int>(length);
}

void ThreadBorderRouter::queue_rcp_led_action_(uint8_t bit, bool enabled) {
  uint8_t previous = rcp_led_pending_.load();
  uint8_t next;
  do {
    next = (previous | (bit << 2)) & ~bit;
    if (enabled) next |= bit;
  } while (!rcp_led_pending_.compare_exchange_weak(previous, next));
}

optional<bool> ThreadBorderRouter::rcp_led_state_(uint8_t bit) const {
  const auto report = thread_br_rcp_led_state();
  if (state_ != State::READY || rcp_fault_ || !report.valid ||
      esp_timer_get_time() - report.received_us > 65000000LL) return {};
  return (report.enabled & bit) != 0;
}

void ThreadBorderRouter::service_rcp_leds_() {
  auto report = thread_br_rcp_led_state();
  if (led_inflight_ && report.valid && report.sequence == led_sequence_ &&
      (report.enabled & (led_inflight_ >> 2)) == (led_inflight_ & (led_inflight_ >> 2)))
    led_inflight_ = 0;
  uint8_t pending = rcp_led_pending_.exchange(0);
  if (pending) {
    const uint8_t mask = pending >> 2;
    led_inflight_ = ((led_inflight_ | pending) & 0x0c) |
                    ((led_inflight_ & 3 & ~mask) | (pending & 3));
    if (++led_sequence_ == 0) ++led_sequence_;
    led_attempts_ = 0;
  }
  const uint32_t now = millis();
  if (led_inflight_ && led_attempts_ >= 3 && now - led_last_attempt_ >= 1000) {
    ESP_LOGW(TAG, "H2 LED action was not confirmed; keeping the last H2 reported state");
    led_inflight_ = 0;
  }
  bool send_action = led_inflight_ && led_attempts_ < 3 &&
                     (led_attempts_ == 0 || now - led_last_attempt_ >= 1000);
  bool query = (!report.valid || esp_timer_get_time() - report.received_us > 65000000LL) &&
               (led_last_query_ == 0 || now - led_last_query_ >= 5000);
  if (!send_action && !query) return;
  if (!esp_openthread_lock_acquire(pdMS_TO_TICKS(20))) return;
  if (send_action) {
    send_rcp_led_control_(1, led_inflight_ >> 2, led_inflight_ & 3, healthy_ && !rcp_fault_, led_sequence_);
    led_last_attempt_ = now;
    ++led_attempts_;
  } else {
    send_rcp_led_control_(2, 0, 0, healthy_ && !rcp_fault_, 0);
    led_last_query_ = now;
  }
  esp_openthread_lock_release();
}

void ThreadBorderRouter::update() {
  if (state_ != State::READY) return;
  const bool healthy = healthy_ && !rcp_fault_;
  const unsigned role = role_;
  static const char *const roles[] = {"disabled", "detached", "child", "router", "leader"};
  if (rcp_connectivity_) rcp_connectivity_->publish_state(healthy);
  if (thread_attached_) thread_attached_->publish_state(healthy && role >= OT_DEVICE_ROLE_CHILD);
  if (thread_role_) thread_role_->publish_state(role < 5 ? roles[role] : "unknown");
  if (rcp_version_) rcp_version_->publish_state(version_);
  if (neighbor_count_) neighbor_count_->publish_state(neighbors_);
  if (child_count_) child_count_->publish_state(children_);
  if (healthy) this->status_clear_warning();
  else this->status_set_warning("RCP communication failure");
}

void ThreadBorderRouter::on_shutdown() {
  state_ = State::STOPPING;
  // Reboot/OTA follows immediately. Do not destroy shared Wi-Fi/mDNS or wait
  // for a potentially long HTTP diagnostic in ESPHome's shutdown hook.
}

void ThreadBorderRouter::dump_config() {
  ESP_LOGCONFIG(TAG, "ESP Thread BR (UART RCP, manual RCP firmware)");
  ESP_LOGCONFIG(TAG, "  UART%d TX=%d RX=%d, %d baud", UART_PORT, tx_, rx_, BAUD_RATE);
  ESP_LOGCONFIG(TAG, "  RESET=%d BOOT=%d; four-wire external coexistence", reset_, boot_);
  ESP_LOGCONFIG(TAG, "  Network: %s, channel %d, PAN ID 0x%04x", CONFIG_OPENTHREAD_NETWORK_NAME,
                CONFIG_OPENTHREAD_NETWORK_CHANNEL, CONFIG_OPENTHREAD_NETWORK_PANID);
  ESP_LOGCONFIG(TAG, "  Force dataset: %s; Web port: %u; authentication: %s", YESNO(force_dataset_),
                web_port_, YESNO(!auth_.empty()));
}
}  // namespace esphome::thread_br
