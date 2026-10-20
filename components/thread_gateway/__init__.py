"""Thread gateway configuration and code generation (referencing thread_br)."""
import ipaddress
import re

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome import automation
from esphome.const import CONF_TRIGGER_ID
from esphome.components import thread_br
from esphome.components import sensor, binary_sensor, switch, number, button, cover, valve, lock, text
from esphome.core import CORE, CoroPriority, coroutine_with_priority
from .api_compat import validate_api_support, install_api_overlay
from esphome.core.entity_helpers import (
    register_device_class, register_unit_of_measurement, register_icon,
)

DEPENDENCIES = ["esp32", "thread_br"]
AUTO_LOAD = ['sensor', 'binary_sensor', 'text_sensor', 'switch', 'number', 'select', 'button', 'text', 'light', 'fan', 'cover', 'climate', 'lock', 'valve', 'thread_entity_bridge']
ns = cg.esphome_ns.namespace("thread_gateway")
ThreadGateway = ns.class_("ThreadGateway", cg.Component)
REPORT_TRIGGERS = {
    "sensor": (ns.class_("NodeSensorUpdateTrigger", automation.Trigger.template(cg.float_)), cg.float_),
    "binary_sensor": (ns.class_("NodeBinaryUpdateTrigger", automation.Trigger.template(cg.bool_)), cg.bool_),
    "text_sensor": (ns.class_("NodeTextUpdateTrigger", automation.Trigger.template(cg.std_string)), cg.std_string),
}

UNITS = [
    "°C", "°F", "%", "lx", "V", "mV", "A", "mA", "µA", "W", "mW",
    "Wh", "kWh", "Ah", "mAh", "dBm", "dB", "Pa", "hPa", "kPa",
    "ppm", "ppb", "µg/m³", "mg/m³", "m", "cm", "mm", "mm/h",
    "m/s", "km/h", "Hz", "kHz", "Ω", "kΩ", "s", "min", "h",
    "B", "kB", "MB", "g", "kg", "L", "mL", "L/min", "°", "K",
]
ICONS = [
    "mdi:thermometer", "mdi:water-percent", "mdi:brightness-5",
    "mdi:battery", "mdi:flash", "mdi:wifi", "mdi:wifi-settings",
    "mdi:identifier", "mdi:information-outline", "mdi:new-box",
]
DEVICE_CLASSES = sorted(set(value for module in (sensor, binary_sensor, switch, number, button, cover, valve, lock, text) for value in getattr(module, "DEVICE_CLASSES", [])))


def identifier(value):
    value = cv.string_strict(value)
    if not re.fullmatch(r"[a-zA-Z0-9_-]{1,32}", value):
        raise cv.Invalid("Expected 1..32 ASCII letters, digits, '_' or '-'")
    return value


def multicast_address(value):
    try:
        address = ipaddress.IPv6Address(cv.string_strict(value))
    except ValueError as err:
        raise cv.Invalid(str(err)) from err
    if not address.is_multicast or (address.packed[1] & 15) != 3:
        raise cv.Invalid("Discovery requires a realm-local (scope 3) IPv6 multicast address")
    # Use an application group, not the groups reserved by the Thread stack.
    if address.packed[1] >> 4 != 1:
        raise cv.Invalid("Use a transient application multicast group such as ff13::8000:8562")
    return str(address)


def positive_seconds(value):
    value = cv.positive_time_period_seconds(value)
    if not 1 <= value.total_seconds <= 86400:
        raise cv.Invalid("Expected a duration from 1s through 24h")
    return value


def nvs_capacity(value):
    if isinstance(value, bool):
        raise cv.Invalid("Expected a byte count or size such as 300KB")
    if isinstance(value, int):
        size = value
    else:
        match = re.fullmatch(r"([0-9]+)\s*(B|KB|KIB|MB|MIB)?", cv.string_strict(value).strip(), re.IGNORECASE)
        if not match:
            raise cv.Invalid("Expected a byte count or size such as 300KB")
        unit = (match.group(2) or "B").upper()
        size = int(match.group(1)) * {"B":1, "KB":1000, "KIB":1024, "MB":1000000, "MIB":1048576}[unit]
    return cv.int_range(min=4096, max=508000)(size)


ENTITY_CAPACITY_KEYS = ('sensors', 'binary_sensors', 'text_sensors', 'switches', 'numbers', 'selects', 'buttons', 'texts', 'lights', 'fans', 'covers', 'climates', 'locks', 'valves')


def subdevice_capacity(config):
    return min(128, sum(config["capacity"][key] for key in ENTITY_CAPACITY_KEYS))


@coroutine_with_priority(CoroPriority.WORKAROUNDS)
async def reserve_subdevices(capacity):
    # Core's priority-100 job has already registered manually configured devices.
    local_count = len(CORE.config.get("esphome", {}).get("devices", []))
    CORE.defines = {d for d in CORE.defines if d.name != "ESPHOME_DEVICE_COUNT"}
    cg.add_define("USE_DEVICES")
    cg.add_define("ESPHOME_DEVICE_COUNT", local_count + capacity)
    if "api" in CORE.config:
        install_api_overlay()


def validate_thread_gateway(config):
    if config["discovery"]["port"] == config["udp"]["port"]:
        raise cv.Invalid("Discovery and data ports must be different")
    total = sum(config["capacity"][key] for key in ENTITY_CAPACITY_KEYS)
    if not 1 <= total <= 2048:
        raise cv.Invalid("Reserve 1..2048 total entities")
    try:
        validate_api_support()
    except ValueError as err:
        raise cv.Invalid(str(err)) from err
    return config


NODE_UPDATE_AUTOMATION = cv.ensure_list(cv.typed_schema({
    kind: automation.validate_automation({
        cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(trigger),
        cv.Required("node_id"): identifier,
        cv.Required("entity_id"): identifier,
    }, single=True)
    for kind, (trigger, _) in REPORT_TRIGGERS.items()
}))


THREAD_GATEWAY_SCHEMA = cv.All(cv.Schema({
    cv.GenerateID(): cv.declare_id(ThreadGateway),
    cv.Required("thread_br_id"): cv.use_id(thread_br.ThreadBorderRouter),
    cv.Optional("reed_heartbeat_misses", default=3): cv.int_range(min=2, max=10),
    cv.Optional("gateway_id"): identifier,
    cv.Optional("entity_auto_register", default=False): cv.boolean,
    cv.Optional("on_node_update"): NODE_UPDATE_AUTOMATION,
    cv.Optional("discovery", default={}): cv.Schema({
        cv.Optional("multicast_address", default="ff13::8000:8562"): multicast_address,
        cv.Optional("port", default=18512): cv.port,
        cv.Optional("address_ttl", default="1h"): positive_seconds,
        cv.Optional("announce_interval", default="60s"): positive_seconds,
    }),
    cv.Optional("udp", default={}): cv.Schema({
        cv.Optional("port", default=18513): cv.port,
    }),
    cv.Optional("capacity", default={}): cv.Schema({
        cv.Optional("sensors", default=32): cv.int_range(min=0, max=1024),
        cv.Optional("binary_sensors", default=16): cv.int_range(min=0, max=1024),
        cv.Optional("text_sensors", default=16): cv.int_range(min=0, max=1024),
        cv.Optional("switches", default=0): cv.int_range(min=0, max=1024),
        cv.Optional("numbers", default=0): cv.int_range(min=0, max=1024),
        cv.Optional("selects", default=0): cv.int_range(min=0, max=1024),
        cv.Optional("buttons", default=0): cv.int_range(min=0, max=1024),
        cv.Optional("texts", default=0): cv.int_range(min=0, max=1024),
        cv.Optional("lights", default=0): cv.int_range(min=0, max=1024),
        cv.Optional("fans", default=0): cv.int_range(min=0, max=1024),
        cv.Optional("covers", default=0): cv.int_range(min=0, max=1024),
        cv.Optional("climates", default=0): cv.int_range(min=0, max=1024),
        cv.Optional("locks", default=0): cv.int_range(min=0, max=1024),
        cv.Optional("valves", default=0): cv.int_range(min=0, max=1024),
        cv.Optional("nvs", default="300KB"): nvs_capacity,
    }),
}).extend(cv.COMPONENT_SCHEMA), validate_thread_gateway)


CONFIG_SCHEMA = THREAD_GATEWAY_SCHEMA


async def to_code(config):
    parent = await cg.get_variable(config["thread_br_id"])
    var = cg.new_Pvariable(config["id"])
    await cg.register_component(var, config)
    cg.add_define("USE_THREAD_GATEWAY")
    CORE.add_job(reserve_subdevices, subdevice_capacity(config))
    cg.add_define("USE_ENTITY_DEVICE_CLASS")
    cg.add_define("USE_ENTITY_UNIT_OF_MEASUREMENT")
    cg.add_define("USE_ENTITY_ICON")
    # No placeholder entities are exposed. Account for the App registry slots
    # before core's FINAL job emits the StaticVector capacity macros.
    for key, platform in zip(ENTITY_CAPACITY_KEYS, ['sensor', 'binary_sensor', 'text_sensor', 'switch', 'number', 'select', 'button', 'text', 'light', 'fan', 'cover', 'climate', 'lock', 'valve']):
        if config["capacity"][key] == 0:
            continue
        cg.add_define("USE_" + platform.upper())
        for _ in range(config["capacity"][key]):
            CORE.register_platform_component(platform, var)
    cg.add(var.set_parent(parent))
    cg.add(var.set_reed_heartbeat_misses(config["reed_heartbeat_misses"]))
    cg.add(parent.set_thread_gateway(var))
    if "gateway_id" in config:
        cg.add(var.set_gateway_id(config["gateway_id"]))
    cg.add(var.set_auto_register(config["entity_auto_register"]))
    d = config["discovery"]
    cg.add(var.set_discovery(d["multicast_address"], d["port"],
                            d["address_ttl"].total_seconds,
                            d["announce_interval"].total_seconds))
    cg.add(var.set_data_port(config["udp"]["port"]))
    c = config["capacity"]
    cg.add(var.set_capacity(c["sensors"], c["binary_sensors"], c["text_sensors"]))
    cg.add(var.set_control_capacity(3, c["switches"]))
    cg.add(var.set_control_capacity(4, c["numbers"]))
    cg.add(var.set_control_capacity(5, c["selects"]))
    cg.add(var.set_control_capacity(6, c["buttons"]))
    cg.add(var.set_control_capacity(7, c["texts"]))
    cg.add(var.set_control_capacity(8, c["lights"]))
    cg.add(var.set_control_capacity(9, c["fans"]))
    cg.add(var.set_control_capacity(10, c["covers"]))
    cg.add(var.set_control_capacity(11, c["climates"]))
    cg.add(var.set_control_capacity(12, c["locks"]))
    cg.add(var.set_control_capacity(13, c["valves"]))
    cg.add(var.set_registry_limit(c["nvs"]))
    for value in DEVICE_CLASSES:
        cg.add(var.add_metadata(0, value, register_device_class(value)))
    for value in UNITS:
        cg.add(var.add_metadata(1, value, register_unit_of_measurement(value)))
    for value in ICONS:
        cg.add(var.add_metadata(2, value, register_icon(value)))
    for conf in config.get("on_node_update", []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var, conf["node_id"], conf["entity_id"])
        await automation.build_automation(trigger, [(REPORT_TRIGGERS[conf["type"]][1], "x")], conf)
    return var
