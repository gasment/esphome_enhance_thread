"""Continuous Thread UDP transport for existing ESPHome entities (ESP32/IDF)."""
import re
import ipaddress
from esphome import automation
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.const import CONF_ID
from esphome.components import openthread, sensor, binary_sensor, text_sensor, switch, number, select, button, text, light, fan, cover, climate, lock, valve
from esphome.components.esp32 import include_builtin_idf_component
from esphome.core import CORE
DEPENDENCIES = ["esp32", "openthread"]
AUTO_LOAD = ["thread_entity_bridge"]
ns = cg.esphome_ns.namespace("thread_reed_transporter")
ThreadReedTransporter = ns.class_("ThreadReedTransporter", cg.Component)
TYPES = {
    "sensors":sensor.Sensor,"binary_sensors":binary_sensor.BinarySensor,"text_sensors":text_sensor.TextSensor,
    "switches":switch.Switch,"numbers":number.Number,"selects":select.Select,"buttons":button.Button,
    "texts":text.Text,"lights":light.LightState,"fans":fan.Fan,"covers":cover.Cover,
    "climates":climate.Climate,"locks":lock.Lock,"valves":valve.Valve,
}
def identifier(value):
    value=cv.string_strict(value)
    if not re.fullmatch(r"[a-zA-Z0-9_-]{1,32}",value): raise cv.Invalid("Expected 1..32 ASCII letters, digits, '_' or '-'")
    return value

def multicast(value):
    try: address=ipaddress.IPv6Address(cv.string_strict(value))
    except ValueError as err: raise cv.Invalid("Invalid IPv6 multicast address") from err
    if not address.is_multicast or address.packed[1]!=0x13: raise cv.Invalid("Use transient realm-local multicast, e.g. ff13::8000:8562")
    return str(address)

def validate(config):
    ids=[str(x) for key in TYPES for x in config[key]]
    if not 1<=len(ids)<=128: raise cv.Invalid("Configure 1..128 existing entity IDs")
    if len(set(ids))!=len(ids): raise cv.Invalid("Each entity ID may only appear once")
    for x in ids: identifier(x)
    if config["heartbeat_interval"].total_milliseconds>15000: raise cv.Invalid("heartbeat_interval must be <=15s (gateway lease 30s)")
    if config["heartbeat_interval"].total_milliseconds<1000: raise cv.Invalid("heartbeat_interval must be >=1s")
    if not 100<=config["ack"]["timeout"].total_milliseconds<=60000: raise cv.Invalid("ack.timeout must be 100ms..60s")
    if config["batch_delay"].total_milliseconds>10000: raise cv.Invalid("batch_delay must be <=10s")
    if config["full_sync_interval"].total_milliseconds<1000: raise cv.Invalid("full_sync_interval must be >=1s")
    return config
CONFIG_SCHEMA=cv.All(cv.Schema({
    cv.GenerateID():cv.declare_id(ThreadReedTransporter),
    cv.Required("openthread_id"):cv.use_id(openthread.OpenThreadComponent),
    cv.Required("node_id"):identifier,
    cv.Optional("name"):cv.Length(min=1,max=96),
    cv.Optional("manifest_revision",default=1):cv.int_range(min=1,max=4294967295),
    cv.Optional("gateway_id",default=""):cv.Any("",identifier),
    cv.Optional("discovery",default={}):cv.Schema({
        cv.Optional("multicast_address",default="ff13::8000:8562"):multicast,
        cv.Optional("port",default=18512):cv.port,
    }),
    cv.Optional("heartbeat_interval",default="10s"):cv.positive_time_period_milliseconds,
    cv.Optional("batch_delay",default="100ms"):cv.positive_time_period_milliseconds,
    cv.Optional("full_sync_interval",default="60s"):cv.positive_time_period_milliseconds,
    cv.Optional("ack",default={}):cv.Schema({
        cv.Optional("timeout",default="2s"):cv.positive_time_period_milliseconds,
        cv.Optional("resend",default=2):cv.int_range(min=0,max=10),
    }),
    **{cv.Optional(k,default=[]):cv.ensure_list(cv.use_id(t)) for k,t in TYPES.items()},
}).extend(cv.COMPONENT_SCHEMA),validate)
async def to_code(config):
    include_builtin_idf_component("json")
    cg.add_define("USE_ENTITY_DEVICE_CLASS")
    cg.add_define("USE_ENTITY_UNIT_OF_MEASUREMENT")
    cg.add_define("USE_ENTITY_ICON")
    var=cg.new_Pvariable(config[CONF_ID]); await cg.register_component(var,config)
    ot=await cg.get_variable(config["openthread_id"])
    cg.add(var.set_openthread(ot))
    name=config.get("name",CORE.config["esphome"].get("friendly_name") or CORE.name)
    cg.add(var.set_node(config["node_id"],name,config["manifest_revision"]))
    cg.add(var.set_gateway_id(config["gateway_id"]))
    cg.add(var.set_discovery(config["discovery"]["multicast_address"],config["discovery"]["port"]))
    cg.add(var.set_timing(config["heartbeat_interval"],config["batch_delay"],config["full_sync_interval"],config["ack"]["timeout"],config["ack"]["resend"]))
    for key in TYPES:
        for ref in config[key]:
            obj=await cg.get_variable(ref)
            cg.add(var.add_entity(str(ref),obj))


def final_validate(config):
    from esphome import final_validate as fv
    full=fv.full_config.get()
    ot=full.get("openthread",{})
    if ot.get("device_type","FTD")!="FTD":
        raise cv.Invalid("thread_reed_transporter requires openthread.device_type: FTD; use thread_sed_transporter for sleepy nodes")
    return config
FINAL_VALIDATE_SCHEMA=final_validate


DiscoveryAction = ns.class_("DiscoveryAction", automation.Action)
DISCOVERY_ACTION_SCHEMA = automation.maybe_simple_id({
    cv.Required(CONF_ID): cv.use_id(ThreadReedTransporter),
})


@automation.register_action("thread_reed_transporter.discovery", DiscoveryAction,
                            DISCOVERY_ACTION_SCHEMA, synchronous=True)
async def discovery_to_code(config, action_id, template_arg, args):
    parent = await cg.get_variable(config[CONF_ID])
    return cg.new_Pvariable(action_id, template_arg, parent)
