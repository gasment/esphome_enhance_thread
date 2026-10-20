"""ESP32-S3 + UART RCP Thread border router (ESPHome 2026.9+ / IDF 5.5)."""

import base64
import ipaddress
from pathlib import Path

import esphome.codegen as cg
import esphome.config_validation as cv
import esphome.final_validate as fv
from esphome import pins
from esphome.components import binary_sensor, sensor, text_sensor
from esphome.components.socket import SocketType, consume_sockets
from esphome.components.esp32 import (
    VARIANT_ESP32S3,
    add_idf_component,
    add_idf_sdkconfig_option,
    include_builtin_idf_component,
    only_on_variant,
    require_vfs_dir,
    require_vfs_select,
)
from esphome.const import CONF_ID
from esphome.core import CORE, CoroPriority, coroutine_with_priority

DEPENDENCIES = ["esp32", "wifi"]
AUTO_LOAD = ["network", "mdns", "binary_sensor", "sensor", "text_sensor"]
CONFLICTS_WITH = ["openthread"]

ns = cg.esphome_ns.namespace("thread_br")
ThreadBorderRouter = ns.class_("ThreadBorderRouter", cg.PollingComponent)


def gateway_web_enabled(full_config):
    return "thread_gateway" in full_config


def fixed_hex(length):
    def validate(value):
        # YAML accepts the same 0x-prefixed integers as ESPHome OpenThread.
        if isinstance(value, int):
            if value < 0 or value >= 1 << (length * 8):
                raise cv.Invalid(f"Expected a {length}-byte unsigned value")
            return f"{value:0{length * 2}x}"
        value = cv.string_strict(value).lower().removeprefix("0x")
        if len(value) != length * 2 or any(c not in "0123456789abcdef" for c in value):
            raise cv.Invalid(f"Expected exactly {length * 2} hexadecimal digits")
        return value
    return validate


def network_name(value):
    value = cv.string_strict(value)
    if not 1 <= len(value.encode("utf-8")) <= 16 or "\0" in value:
        raise cv.Invalid("Thread network_name must be 1..16 UTF-8 bytes")
    return value


def mesh_prefix(value):
    try:
        prefix = ipaddress.IPv6Network(cv.string_strict(value), strict=True)
    except ValueError as err:
        raise cv.Invalid(str(err)) from err
    if prefix.prefixlen != 64 or prefix.network_address.packed[0] != 0xFD:
        raise cv.Invalid("mesh_local_prefix must be an fd00::/8 ULA /64")
    return str(prefix)


def validate_config(config):
    require_vfs_select()
    # IDF 5.5 Spinel calls access("/dev/uart/N") before registering UART VFS.
    require_vfs_dir()
    web = config["web"]
    if bool(web["user"]) != bool(web["passwd"]):
        raise cv.Invalid("Set both web.user and web.passwd, or leave both empty")
    if ":" in web["user"]:
        raise cv.Invalid("web.user cannot contain ':'")
    credentials = f'{web["user"]}:{web["passwd"]}'.encode()
    if len(base64.b64encode(credentials)) + 6 >= 384:
        raise cv.Invalid("Web credentials are too long for the Authorization header")
    return config


THREAD_NETWORK_SCHEMA = cv.Schema({
    cv.Required("channel"): cv.int_range(min=11, max=26),
    cv.Required("network_name"): network_name,
    cv.Required("network_key"): fixed_hex(16),
    # Kconfig declares PANID as hex: preserve HexInt through code generation.
    cv.Required("pan_id"): cv.hex_int_range(min=0, max=0xFFFE),
    cv.Required("ext_pan_id"): fixed_hex(8),
    cv.Required("pskc"): fixed_hex(16),
    cv.Required("mesh_local_prefix"): mesh_prefix,
    cv.Optional("force_dataset", default=False): cv.boolean,
})

WEB_SCHEMA = cv.Schema({
    cv.Optional("port", default=80): cv.port,
    cv.Optional("user", default=""): cv.string_strict,
    cv.Optional("passwd", default=""): cv.string_strict,
})

def validate_rcp(config):
    configured_pins = [config[key]["number"] for key in (
        "uart_tx", "uart_rx", "rcp_reset_pin", "rcp_boot_pin",
        "coex_request_pin", "coex_priority_pin", "coex_grant_pin", "coex_tx_line_pin",
    ) if key in config]
    if len(configured_pins) != len(set(configured_pins)):
        raise cv.Invalid("UART, RESET, BOOT and coexistence pins must be distinct")
    return config


RCP_SCHEMA = cv.All(cv.Schema({
    cv.Required("uart_tx"): pins.internal_gpio_output_pin_schema,
    cv.Required("uart_rx"): pins.internal_gpio_input_pin_schema,
    cv.Required("rcp_reset_pin"): pins.internal_gpio_output_pin_schema,
    cv.Required("rcp_boot_pin"): pins.internal_gpio_output_pin_schema,
    cv.Optional("coex_request_pin", default=12): pins.internal_gpio_input_pin_schema,
    cv.Optional("coex_priority_pin", default=10): pins.internal_gpio_input_pin_schema,
    cv.Optional("coex_grant_pin", default=13): pins.internal_gpio_output_pin_schema,
    cv.Optional("coex_tx_line_pin", default=11): pins.internal_gpio_output_pin_schema,
    cv.Optional("rcp_connectivity"): binary_sensor.binary_sensor_schema(
        device_class="connectivity"),
}), validate_rcp)

CONFIG_SCHEMA = cv.All(
    cv.Schema({
        cv.GenerateID(): cv.declare_id(ThreadBorderRouter),
        cv.Required("rcp"): RCP_SCHEMA,
        cv.Optional("web", default={}): WEB_SCHEMA,
        cv.Optional("used_psram", default=True): cv.boolean,
        cv.Required("thread_network_config"): THREAD_NETWORK_SCHEMA,
        cv.Optional("thread_attached"): binary_sensor.binary_sensor_schema(
            device_class="connectivity", entity_category="diagnostic"),
        cv.Optional("thread_role"): text_sensor.text_sensor_schema(entity_category="diagnostic"),
        cv.Optional("rcp_version"): text_sensor.text_sensor_schema(entity_category="diagnostic"),
        cv.Optional("neighbor_count"): sensor.sensor_schema(accuracy_decimals=0, entity_category="diagnostic"),
        cv.Optional("child_count"): sensor.sensor_schema(accuracy_decimals=0, entity_category="diagnostic"),
    }).extend(cv.polling_component_schema("15s")),
    cv.only_on_esp32,
    only_on_variant(supported=[VARIANT_ESP32S3]),
    validate_config,
    consume_sockets(5, "thread_br"),
    consume_sockets(1, "thread_br", SocketType.TCP_LISTEN),
    consume_sockets(3, "thread_br", SocketType.UDP),
)


def final_validate(config):
    full = fv.full_config.get()
    rcp = config["rcp"]
    if not full.get("network", {}).get("enable_ipv6", False):
        raise cv.Invalid("Set network.enable_ipv6: true")
    version = full["esp32"]["framework"]["version"]
    if not str(version).startswith("5.5."):
        raise cv.Invalid("This first release requires ESP-IDF 5.5.x")
    if config["used_psram"] and "psram" not in full:
        raise cv.Invalid("used_psram: true requires the psram component")
    if "esp32_ble" in full:
        raise cv.Invalid("ESP32-S3 external coexistence requires disabling BLE (including esp32_improv)")
    if "web_server" in full and config["web"]["port"] == full["web_server"]["port"]:
        raise cv.Invalid("BR web.port must differ from ESPHome web_server.port")
    for key in ("uart_tx", "uart_rx", "rcp_reset_pin", "rcp_boot_pin",
                "coex_request_pin", "coex_priority_pin", "coex_grant_pin", "coex_tx_line_pin"):
        if key in rcp and rcp[key].get("inverted", False):
            raise cv.Invalid(f"rcp.{key} must not be inverted")
    for uart in full.get("uart", []):
        for key in ("tx_pin", "rx_pin"):
            if key in uart and uart[key]["number"] in (rcp["uart_tx"]["number"], rcp["uart_rx"]["number"]):
                raise cv.Invalid("The Spinel UART pins cannot be shared with ESPHome uart")


FINAL_VALIDATE_SCHEMA = final_validate


@coroutine_with_priority(CoroPriority.COMPONENT)
async def to_code(config):
    network = config["thread_network_config"]
    rcp = config["rcp"]
    web = config["web"]
    # Register pins with ESPHome, but let IDF own their UART/coex drivers.
    for key in ("uart_tx", "uart_rx", "rcp_reset_pin", "rcp_boot_pin",
                "coex_request_pin", "coex_priority_pin", "coex_grant_pin", "coex_tx_line_pin"):
        if key in rcp:
            await cg.gpio_pin_expression(rcp[key])
    for name in ("openthread", "esp_http_server", "json", "esp_coex", "esp_driver_uart", "esp_driver_gpio"):
        include_builtin_idf_component(name)
    add_idf_component(name="thread_br_web", path=str(Path(__file__).parent / "thread_br_web"))
    add_idf_component(name="thread_br_rcp", path=str(Path(__file__).parent / "thread_br_rcp"))
    options = {
        "CONFIG_THREAD_BR_GATEWAY_WEB": gateway_web_enabled(CORE.config),
        "CONFIG_OPENTHREAD_ENABLED": True,
        "CONFIG_OPENTHREAD_FTD": True,
        "CONFIG_OPENTHREAD_BORDER_ROUTER": True,
        "CONFIG_OPENTHREAD_RADIO_SPINEL_UART": True,
        "CONFIG_OPENTHREAD_RADIO_NATIVE": False,
        "CONFIG_OPENTHREAD_RADIO_TREL": False,
        "CONFIG_OPENTHREAD_CLI": False,
        "CONFIG_OPENTHREAD_CONSOLE_ENABLE": False,
        "CONFIG_OPENTHREAD_COMMISSIONER": False,
        "CONFIG_OPENTHREAD_JOINER": False,
        "CONFIG_OPENTHREAD_TASK_SIZE": 8192,
        "CONFIG_MBEDTLS_CMAC_C": True,
        "CONFIG_MBEDTLS_SSL_PROTO_DTLS": True,
        "CONFIG_MBEDTLS_KEY_EXCHANGE_ECJPAKE": True,
        "CONFIG_MBEDTLS_ECJPAKE_C": True,
        "CONFIG_OPENTHREAD_NETWORK_CHANNEL": network["channel"],
        "CONFIG_OPENTHREAD_NETWORK_NAME": network["network_name"],
        "CONFIG_OPENTHREAD_NETWORK_MASTERKEY": network["network_key"],
        "CONFIG_OPENTHREAD_NETWORK_PANID": network["pan_id"],
        "CONFIG_OPENTHREAD_NETWORK_EXTPANID": network["ext_pan_id"],
        "CONFIG_OPENTHREAD_NETWORK_PSKC": network["pskc"],
        "CONFIG_OPENTHREAD_MESH_LOCAL_PREFIX": network["mesh_local_prefix"],
        "CONFIG_LWIP_IPV6_FORWARD": True,
        "CONFIG_LWIP_IPV6_NUM_ADDRESSES": 12,
        "CONFIG_LWIP_MULTICAST_PING": True,
        "CONFIG_LWIP_NETIF_STATUS_CALLBACK": True,
        "CONFIG_LWIP_HOOK_IP6_ROUTE_DEFAULT": True,
        "CONFIG_LWIP_HOOK_ND6_GET_GW_DEFAULT": True,
        "CONFIG_LWIP_HOOK_IP6_INPUT_CUSTOM": True,
        "CONFIG_LWIP_HOOK_IP6_SELECT_SRC_ADDR_CUSTOM": True,
        "CONFIG_LWIP_IPV6_AUTOCONFIG": True,
        "CONFIG_LWIP_FORCE_ROUTER_FORWARDING": True,
        "CONFIG_MDNS_MULTIPLE_INSTANCE": True,
        "CONFIG_MDNS_ENABLE_BROWSE": True,
        "CONFIG_MDNS_MAX_SERVICES": 128 if config["used_psram"] else 64,
        "CONFIG_MDNS_MEMORY_ALLOC_SPIRAM": config["used_psram"],
        "CONFIG_MDNS_MEMORY_ALLOC_INTERNAL": not config["used_psram"],
        "CONFIG_MDNS_MEMORY_CUSTOM_IMPL": False,
        # Keep the task stack internal; only mDNS dynamic data follows used_psram.
        "CONFIG_MDNS_TASK_CREATE_FROM_SPIRAM": False,
        "CONFIG_MDNS_TASK_CREATE_FROM_INTERNAL": True,
        "CONFIG_HTTPD_MAX_REQ_HDR_LEN": 1024,
        "CONFIG_ESP_SYSTEM_EVENT_TASK_STACK_SIZE": 4096,
        "CONFIG_ESP_COEX_EXTERNAL_COEXIST_ENABLE": True,
    }
    for key, value in options.items():
        add_idf_sdkconfig_option(key, value)
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    cg.add(var.set_uart_pins(rcp["uart_tx"]["number"], rcp["uart_rx"]["number"]))
    cg.add(var.set_rcp_pins(rcp["rcp_reset_pin"]["number"], rcp["rcp_boot_pin"]["number"]))
    cg.add(var.set_force_dataset(network["force_dataset"]))
    auth = ""
    if web["user"]:
        auth = "Basic " + base64.b64encode(f'{web["user"]}:{web["passwd"]}'.encode()).decode()
    cg.add(var.set_web(web["port"], auth, config["used_psram"]))
    cg.add(var.set_coexistence_pins(*[rcp[key]["number"] for key in (
        "coex_request_pin", "coex_priority_pin", "coex_grant_pin", "coex_tx_line_pin")]))
    for key, source in (("rcp_connectivity", rcp), ("thread_attached", config)):
        if key in source:
            entity = await binary_sensor.new_binary_sensor(source[key])
            cg.add(getattr(var, f"set_{key}")(entity))
    for key in ("thread_role", "rcp_version"):
        if key in config:
            entity = await text_sensor.new_text_sensor(config[key])
            cg.add(getattr(var, f"set_{key}")(entity))
    for key in ("neighbor_count", "child_count"):
        if key in config:
            entity = await sensor.new_sensor(config[key])
            cg.add(getattr(var, f"set_{key}")(entity))

