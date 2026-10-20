"""Manual sensor acquisition and RTC threshold reporting over Thread UDP."""
import hashlib
import json
import math
import re
import ipaddress

from esphome import automation, preferences
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import sensor, binary_sensor, text_sensor, openthread
from esphome.components import switch, number, select, button, text
from esphome.components.esp32 import include_builtin_idf_component
from esphome.const import CONF_ID, CONF_TRIGGER_ID
from esphome.core import CORE

DEPENDENCIES = ["esp32", "openthread"]
AUTO_LOAD = ["sensor", "binary_sensor", "text_sensor", "preferences", "thread_entity_bridge"]
ns = cg.esphome_ns.namespace("thread_sed_transporter")
ThreadSedTransporter = ns.class_("ThreadSedTransporter", cg.Component)
ReportSensor = ns.class_("ReportSensor")
ReportBinarySensor = ns.class_("ReportBinarySensor")
ReportTextSensor = ns.class_("ReportTextSensor")
ReportAction = ns.class_("ReportAction", automation.Action)
ReportSuccessTrigger = ns.class_("ReportSuccessTrigger", automation.Trigger.template(cg.uint32))
ReportFailTrigger = ns.class_("ReportFailTrigger", automation.Trigger.template(cg.uint32))
ReportCompleteTrigger = ns.class_("ReportCompleteTrigger", automation.Trigger.template(
    cg.bool_, cg.uint32, cg.uint32, cg.uint32, cg.uint32))


def identifier(value):
    value = cv.string_strict(value)
    if not re.fullmatch(r"[a-zA-Z0-9_-]{1,32}", value):
        raise cv.Invalid("Expected 1..32 ASCII letters, digits, '_' or '-'")
    return value


def multicast(value):
    try:
        address = ipaddress.IPv6Address(cv.string_strict(value))
    except ValueError as err:
        raise cv.Invalid(str(err)) from err
    if not address.is_multicast or address.packed[1] != 0x13:
        raise cv.Invalid("Use a transient realm-local multicast address, e.g. ff13::8000:8562")
    return str(address)


def delta(value):
    value = cv.float_(value)
    if not math.isfinite(value) or not 0 <= value <= 3.402823466e38:
        raise cv.Invalid("delta must be finite and non-negative")
    return value


def entry_schema(source_type, record_type, extra):
    return cv.maybe_simple_value(cv.Schema({
        cv.Required(CONF_ID): cv.use_id(source_type),
        cv.GenerateID("entry_id"): cv.declare_id(record_type),
        cv.Optional("source_components", default=[]): cv.ensure_list(cv.use_id(cg.PollingComponent)),
        **extra,
    }), key=CONF_ID)


ENTRY_SCHEMA = entry_schema(sensor.Sensor, ReportSensor, {cv.Required("delta"): delta})
BINARY_SCHEMA = entry_schema(binary_sensor.BinarySensor, ReportBinarySensor, {})
TEXT_SCHEMA = entry_schema(text_sensor.TextSensor, ReportTextSensor,
                          {cv.Optional("max_length", default=32): cv.int_range(min=1, max=128)})

CONTROL_TYPES = {
    "switches": ("switch", switch.Switch),
    "numbers": ("number", number.Number),
    "selects": ("select", select.Select),
    "buttons": ("button", button.Button),
    "text_inputs": ("input_text", text.Text),
}


def control_schema(source_type):
    return cv.maybe_simple_value({cv.Required(CONF_ID): cv.use_id(source_type)}, key=CONF_ID)


def effective_components(entry, full_config):
    if entry["source_components"]:
        return entry["source_components"]
    path = full_config.get_path_for_id(entry[CONF_ID])
    declaration = full_config.get_config_for_path(path)
    if declaration.type.inherits_from(cg.PollingComponent) and declaration.type.inherits_from(sensor.Sensor):
        return [declaration]
    return []


def source_metadata(entry, full_config):
    path = full_config.get_path_for_id(entry[CONF_ID])[:-1]
    config = full_config.get_config_for_path(path)
    fields = ("name", "unit_of_measurement", "device_class", "icon", "state_class",
              "accuracy_decimals", "entity_category", "disabled_by_default", "force_update",
              "trigger_on_initial_state", "min_value", "max_value", "step", "options", "mode")
    return {field: config[field] for field in fields if field in config}


def validate(config):
    preferences.validate_rtc_storage(True)
    entries = [e for key in ("sensors", "binary_sensors", "text_sensors") for e in config[key]]
    controls = [e for key in CONTROL_TYPES for e in config[key]]
    if not 1 <= len(entries) + len(controls) <= 16:
        raise cv.Invalid("Configure 1..16 total read-only and control entities")
    ids = set()
    for e in entries + controls:
        identity = identifier(str(e[CONF_ID]))
        if identity in ids:
            raise cv.Invalid(f"Duplicate wire entity id: {identity}")
        ids.add(identity)
    size = 12 * (len(config["sensors"]) + len(config["binary_sensors"]))
    size += sum(4 + ((8 + e["max_length"] + 3) // 4) * 4 for e in config["text_sensors"])
    if size > 256:
        raise cv.Invalid(f"Read-only baselines need {size} RTC preference bytes; shared maximum is 256")
    if config["acquisition_delay"].total_milliseconds >= config["timeout"].total_milliseconds:
        raise cv.Invalid("acquisition_delay must be shorter than timeout")
    if not 1 <= config["discovery"]["timeout"].total_milliseconds <= 10000:
        raise cv.Invalid("discovery.timeout must be 1ms..10s")
    if not 1000 <= config["heartbeat_interval"].total_milliseconds <= 15000:
        raise cv.Invalid("heartbeat_interval must be 1s..15s (gateway lease is 30s)")
    if len(config["name"].encode("utf-8")) > 96:
        raise cv.Invalid("Node name must fit 96 UTF-8 bytes")
    return config


CONFIG_SCHEMA = cv.All(cv.Schema({
    cv.GenerateID(): cv.declare_id(ThreadSedTransporter),
    cv.Required("openthread_id"): cv.use_id(openthread.OpenThreadComponent),
    cv.Required("node_id"): identifier,
    cv.Optional("name", default="SED node"): cv.string_strict,
    cv.Optional("manifest_revision", default=1): cv.int_range(min=1, max=4294967295),
    cv.Optional("discovery", default={}): cv.Schema({
        cv.Optional("gateway_id"): identifier,
        cv.Optional("multicast_address", default="ff13::8000:8562"): multicast,
        cv.Optional("port", default=18512): cv.port,
        cv.Optional("timeout", default="1s"): cv.positive_time_period_milliseconds,
    }),
    cv.Optional("acquisition_delay", default="0ms"): cv.positive_time_period_milliseconds,
    cv.Optional("timeout", default="5s"): cv.positive_time_period_milliseconds,
    cv.Optional("full_report_every", default=20): cv.int_range(min=1, max=65535),
    cv.Optional("control_ready", default=False): cv.templatable(cv.boolean),
    cv.Optional("heartbeat_interval", default="10s"): cv.positive_time_period_milliseconds,
    cv.Optional("sensors", default=[]): cv.ensure_list(ENTRY_SCHEMA),
    cv.Optional("binary_sensors", default=[]): cv.ensure_list(BINARY_SCHEMA),
    cv.Optional("text_sensors", default=[]): cv.ensure_list(TEXT_SCHEMA),
    **{cv.Optional(key, default=[]): cv.ensure_list(control_schema(t)) for key, (_, t) in CONTROL_TYPES.items()},
    cv.Optional("on_report_success"): automation.validate_automation({
        cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(ReportSuccessTrigger),
    }),
    cv.Optional("on_report_fail"): automation.validate_automation({
        cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(ReportFailTrigger),
    }),
    cv.Optional("on_complete"): automation.validate_automation({
        cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(ReportCompleteTrigger),
    }),
}).extend(cv.COMPONENT_SCHEMA), validate)



async def to_code(config):
    preferences.request_rtc_storage()
    include_builtin_idf_component("json")
    for flag in ("USE_ENTITY_DEVICE_CLASS", "USE_ENTITY_UNIT_OF_MEASUREMENT", "USE_ENTITY_ICON"):
        cg.add_define(flag)
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    ot = await cg.get_variable(config["openthread_id"])
    cg.add(var.set_openthread(ot))
    cg.add(var.set_node(config["node_id"], config["name"], config["manifest_revision"]))
    d = config["discovery"]
    cg.add(var.set_discovery(d.get("gateway_id", ""), d["multicast_address"], d["port"], d["timeout"]))
    cg.add(var.set_timing(config["acquisition_delay"], config["timeout"],
                         config["full_report_every"], config["heartbeat_interval"]))
    ready = await cg.templatable(config["control_ready"], [], cg.bool_)
    if isinstance(config["control_ready"], bool):
        cg.add(var.set_control_ready(ready))
    else:
        cg.add(var.set_control_ready_reader(ready))
    schema_fingerprint = []
    for key, kind in (("sensors", "numeric"), ("binary_sensors", "binary"), ("text_sensors", "text")):
        for entry in config[key]:
            out = cg.new_Pvariable(entry["entry_id"])
            source = await cg.get_variable(entry[CONF_ID])
            cg.add(out.set_identity(str(entry[CONF_ID])))
            components = effective_components(entry, CORE.config)
            cg.add(out.set_source(source, bool(components)))
            if key == "binary_sensors":
                metadata = source_metadata(entry, CORE.config)
                cg.add(out.set_initial_state_metadata(metadata.get("trigger_on_initial_state", True)))
            if key == "text_sensors":
                cg.add(out.set_max_length(entry["max_length"]))
            if key == "sensors":
                cg.add(out.set_delta(entry["delta"]))
            storage_key = int(hashlib.sha256(f"thread_sed_transporter/v2/{kind}/{entry[CONF_ID]}".encode()).hexdigest()[:8], 16)
            cg.add(out.set_storage_key(storage_key))
            cg.add(var.add_sensor(out))
            for component_id in components:
                component = await cg.get_variable(component_id)
                cg.add(var.add_update_component(component))
            schema_fingerprint.append([kind, str(entry[CONF_ID]), entry.get("max_length"), source_metadata(entry, CORE.config)])
    for key, (kind, _) in CONTROL_TYPES.items():
        for entry in config[key]:
            source = await cg.get_variable(entry[CONF_ID])
            cg.add(var.add_control(str(entry[CONF_ID]), source))
            schema_fingerprint.append([kind, str(entry[CONF_ID]), source_metadata(entry, CORE.config)])
    fingerprint = json.dumps([config["node_id"], config["name"], config["manifest_revision"],
                              d.get("gateway_id",""),d["multicast_address"],d["port"],
                              schema_fingerprint], sort_keys=True, default=str)
    cg.add(var.set_storage_identity(int(hashlib.sha256(fingerprint.encode()).hexdigest()[:8],16)))
    for key in ("on_report_success", "on_report_fail"):
        for conf in config.get(key, []):
            trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
            await automation.build_automation(trigger, [(cg.uint32, "report_id")], conf)
    for conf in config.get("on_complete", []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
        await automation.build_automation(trigger, [
            (cg.bool_, "success"), (cg.uint32, "sent"), (cg.uint32, "unchanged"),
            (cg.uint32, "invalid"), (cg.uint32, "errors")], conf)


def ack_timeout(value):
    value = cv.positive_time_period_milliseconds(value)
    if not 1 <= value.total_milliseconds <= 60000:
        raise cv.Invalid("ack.timeout must be 1ms..60s per attempt")
    return value


REPORT_ACTION_SCHEMA = automation.maybe_simple_id({
    cv.Required(CONF_ID): cv.use_id(ThreadSedTransporter),
    cv.Optional("ack"): cv.Schema({
        cv.Optional("timeout", default="2s"): ack_timeout,
        cv.Optional("resend", default=0): cv.int_range(min=0, max=10),
    }),
})


@automation.register_action("thread_sed_transporter.report", ReportAction,
    REPORT_ACTION_SCHEMA, synchronous=True)
async def report_to_code(config, action_id, template_arg, args):
    parent = await cg.get_variable(config[CONF_ID])
    var = cg.new_Pvariable(action_id, template_arg, parent)
    ack = config.get("ack")
    cg.add(var.set_ack_timeout(ack["timeout"].total_milliseconds if ack is not None else 0))
    cg.add(var.set_ack_resend(ack["resend"] if ack is not None else 0))
    return var


DiscoveryAction = ns.class_("DiscoveryAction", automation.Action)
DISCOVERY_ACTION_SCHEMA = automation.maybe_simple_id({
    cv.Required(CONF_ID): cv.use_id(ThreadSedTransporter),
})


@automation.register_action("thread_sed_transporter.discovery", DiscoveryAction,
                            DISCOVERY_ACTION_SCHEMA, synchronous=True)
async def discovery_to_code(config, action_id, template_arg, args):
    parent = await cg.get_variable(config[CONF_ID])
    return cg.new_Pvariable(action_id, template_arg, parent)
