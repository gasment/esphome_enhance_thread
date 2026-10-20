"""Typed, unicast Thread messaging with optional per-action receipt ACK."""
import ipaddress
import math
import re

from esphome import automation
import esphome.codegen as cg
import esphome.config_validation as cv
import esphome.final_validate as fv
from esphome.const import CONF_ID, CONF_TRIGGER_ID
from esphome.core import Lambda

DEPENDENCIES = ["esp32", "openthread"]
ns = cg.esphome_ns.namespace("thread_p2p_transporter")
ThreadP2PTransporter = ns.class_("ThreadP2PTransporter", cg.Component)
Payload = ns.struct("Payload")
ReceiveTrigger = ns.class_("ReceiveTrigger", automation.Trigger.template(cg.uint8, Payload))
SendSuccessTrigger = ns.class_("SendSuccessTrigger", automation.Trigger.template(cg.uint8, cg.uint32))
SendFailTrigger = ns.class_("SendFailTrigger", automation.Trigger.template(cg.uint8, cg.uint32))
SendAction = ns.class_("SendAction", automation.Action)
FlagCondition = ns.class_("FlagCondition", automation.Condition)
PayloadCondition = ns.class_("PayloadCondition", automation.Condition)
PublishAction = ns.class_("PublishAction", automation.Action)
DEFAULT_PORT = 18520
MAX_PAYLOAD = 1208  # 1232-byte UDP datagram minus the 24-byte versioned header.


def target_id(value):
    value = cv.string_strict(value)
    if not re.fullmatch(r"[a-zA-Z0-9_-]{1,32}", value):
        raise cv.Invalid("Target id must contain 1..32 ASCII letters, digits, '_' or '-'")
    return value


def address(value):
    value = cv.string_strict(value)
    if "%" in value:
        raise cv.Invalid("Use a Mesh-Local unicast address without a zone identifier")
    try:
        ip = ipaddress.IPv6Address(value)
    except ValueError as err:
        raise cv.Invalid("Expected an IPv6 address") from err
    if ip.packed[0] != 0xFD:
        raise cv.Invalid("Use an fd00::/8 address in the Thread mesh_local_prefix /64")
    iid = ip.packed[8:]
    if iid == bytes(8) or iid[:6] == bytes.fromhex("000000fffe00"):
        raise cv.Invalid("The subnet-router, RLOC and ALOC address ranges are reserved")
    if iid[:7] == bytes.fromhex("fdffffffffffff") and iid[7] >= 0x80:
        raise cv.Invalid("Reserved subnet anycast interface identifier")
    return str(ip)


def string_payload(value):
    value = cv.string_strict(value)
    if len(value.encode("utf-8")) > MAX_PAYLOAD:
        raise cv.Invalid(f"payload must fit {MAX_PAYLOAD} UTF-8 bytes")
    return value


def float_payload(value):
    if isinstance(value, bool):
        raise cv.Invalid("Boolean is not a float payload")
    value = cv.float_(value)
    if not math.isfinite(value) or abs(value) > 3.4028234663852886e38:
        raise cv.Invalid("Float payload must be a finite IEEE-754 float32")
    return value


PAYLOAD_VALIDATORS = {"bool": cv.boolean, "float": float_payload, "string": string_payload}
PAYLOAD_CPP = {"bool": cg.bool_, "float": cg.float_, "string": cg.std_string}


def validate(config):
    own = ipaddress.IPv6Address(config["address"]) if "address" in config else None
    prefix = ipaddress.IPv6Network((own, 64), strict=False) if own else None
    if own:
        config.setdefault("port", DEFAULT_PORT)
    ids = set()
    for entry in config["send_to_addr_list"]:
        target = ipaddress.IPv6Address(entry["address"])
        if entry["id"] in ids:
            raise cv.Invalid("Target ids must be unique")
        if target == own:
            raise cv.Invalid("A target must not use the local address")
        if prefix is None:
            prefix = ipaddress.IPv6Network((target, 64), strict=False)
        if target not in prefix:
            raise cv.Invalid("All addresses must use the same Mesh-Local /64")
        ids.add(entry["id"])
    return config


CONFIG_SCHEMA = cv.All(cv.Schema({
    cv.GenerateID(): cv.declare_id(ThreadP2PTransporter),
    cv.Optional("address"): address,
    cv.Optional("port"): cv.int_range(min=1, max=65535),
    cv.Optional("send_to_addr_list", default=[]): cv.All(cv.ensure_list(cv.Schema({
        cv.Required("id"): target_id,
        cv.Required("address"): address,
        cv.Optional("port", default=DEFAULT_PORT): cv.int_range(min=1, max=65535),
    })), cv.Length(max=128)),
    cv.Optional("on_receive"): automation.validate_automation({
        cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(ReceiveTrigger),
    }),
    cv.Optional("on_send_success"): automation.validate_automation({
        cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(SendSuccessTrigger),
    }),
    cv.Optional("on_send_fail"): automation.validate_automation({
        cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(SendFailTrigger),
    }),
}).extend(cv.COMPONENT_SCHEMA), validate)


def configured_prefix(ot):
    if ot.get("tlv"):
        raw = bytes.fromhex(ot["tlv"])
        offset = 0
        while offset < len(raw):
            if offset + 2 > len(raw):
                raise cv.Invalid("Malformed OpenThread dataset TLV")
            kind, length = raw[offset:offset + 2]
            offset += 2
            if offset + length > len(raw):
                raise cv.Invalid("Malformed OpenThread dataset TLV")
            if kind == 7:
                if length != 8:
                    raise cv.Invalid("Invalid Mesh-Local Prefix TLV length")
                return ipaddress.IPv6Network((ipaddress.IPv6Address(raw[offset:offset + length] + bytes(8)), 64))
            offset += length
        return None
    if ot.get("mesh_local_prefix"):
        return ipaddress.IPv6Network(ot["mesh_local_prefix"])
    return None  # The active persisted/default dataset is checked again at runtime.



def final_validate(config):
    full = fv.full_config.get()
    ot = full.get("openthread", {})
    if ot.get("device_type", "FTD") == "MTD":
        for key in ("address", "port", "on_receive"):
            if key in config:
                raise cv.Invalid(f"MTD is application-send-only; do not configure {key}")
    elif "address" not in config:
        raise cv.Invalid("FTD must configure its fixed application address")
    prefix = configured_prefix(ot)
    addresses = ([config["address"]] if "address" in config else [])
    addresses.extend(entry["address"] for entry in config["send_to_addr_list"])
    if prefix and any(ipaddress.IPv6Address(value) not in prefix for value in addresses):
        raise cv.Invalid("All addresses must belong to openthread.mesh_local_prefix /64")
    def walk(value):
        if isinstance(value, dict):
            for key, entry in value.items():
                if key == "thread_p2p_transporter.send":
                    validate_send_final(entry)
                walk(entry)
        elif isinstance(value, list):
            for entry in value:
                walk(entry)
    walk(full)
    return config


FINAL_VALIDATE_SCHEMA = final_validate


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    cg.add(var.set_sender_only("address" not in config))
    if "address" in config:
        cg.add(var.set_address(config["address"], config["port"]))
    for entry in config["send_to_addr_list"]:
        cg.add(var.add_target(entry["id"], entry["address"], entry["port"]))
    for key, args in (
        ("on_receive", [(cg.uint8, "send_flag"), (Payload, "payload")]),
        ("on_send_success", [(cg.uint8, "send_flag"), (cg.uint32, "message_id")]),
        ("on_send_fail", [(cg.uint8, "send_flag"), (cg.uint32, "message_id")]),
    ):
        for entry in config.get(key, []):
            trigger = cg.new_Pvariable(entry[CONF_TRIGGER_ID], var)
            await automation.build_automation(trigger, args, entry)


ACK_SCHEMA = cv.Schema({
    cv.Optional("timeout", default="500ms"): cv.All(cv.positive_time_period_milliseconds,
        cv.Range(min=cv.TimePeriod(milliseconds=10), max=cv.TimePeriod(seconds=60))),
    cv.Optional("resend", default=2): cv.int_range(min=0, max=10),
})


def validate_send(config):
    if ("send_to" in config) == ("address" in config):
        raise cv.Invalid("Specify exactly one of send_to and address")
    config["payload"] = cv.templatable(PAYLOAD_VALIDATORS[config["payload_type"]])(config["payload"])
    return config


SEND_SCHEMA = cv.All(cv.Schema({
    cv.GenerateID(): cv.use_id(ThreadP2PTransporter),
    cv.Optional("send_to"): cv.templatable(target_id),
    cv.Optional("address"): cv.templatable(address),
    cv.Optional("port"): cv.templatable(cv.int_range(min=1, max=65535)),
    cv.Required("send_flag"): cv.templatable(cv.int_range(min=0, max=255)),
    cv.Required("payload_type"): cv.one_of("bool", "float", "string", lower=True),
    cv.Required("payload"): lambda value: value,
    cv.Optional("ack"): ACK_SCHEMA,
}), validate_send)


def validate_send_final(config):
    full = fv.full_config.get()
    parent = full.get("thread_p2p_transporter", {})
    if "send_to" in config and not isinstance(config["send_to"], Lambda):
        if config["send_to"] not in {x["id"] for x in parent.get("send_to_addr_list", [])}:
            raise cv.Invalid("send_to does not exist in send_to_addr_list")
    if "address" in config and not isinstance(config["address"], Lambda):
        target = ipaddress.IPv6Address(config["address"])
        prefix = configured_prefix(full.get("openthread", {}))
        if prefix and target not in prefix:
            raise cv.Invalid("Target address must belong to openthread.mesh_local_prefix /64")
        if parent.get("address") == config["address"]:
            raise cv.Invalid("Target address must not be the local address")
    return config


@automation.register_action("thread_p2p_transporter.send", SendAction, SEND_SCHEMA, synchronous=True)
async def send_to_code(config, action_id, template_arg, args):
    parent = await cg.get_variable(config[CONF_ID])
    var = cg.new_Pvariable(action_id, template_arg, parent)
    key = "send_to" if "send_to" in config else "address"
    cg.add(var.set_target(await cg.templatable(config[key], args, cg.std_string)))
    cg.add(var.set_target_is_address(key == "address"))
    if "port" in config:
        cg.add(var.set_port(await cg.templatable(config["port"], args, cg.uint16)))
        cg.add(var.set_has_port(True))
    cg.add(var.set_send_flag(await cg.templatable(config["send_flag"], args, cg.uint8)))
    kind = config["payload_type"]
    cg.add(var.set_payload_kind({"bool": 1, "float": 2, "string": 3}[kind]))
    cg.add(getattr(var, "set_"+kind+"_payload")(
        await cg.templatable(config["payload"], args, PAYLOAD_CPP[kind])))
    if "ack" in config:
        cg.add(var.set_ack(config["ack"]["timeout"].total_milliseconds, config["ack"]["resend"]))
    return var


@automation.register_condition("thread_p2p_transporter.send_flag", FlagCondition, cv.uint8_t)
async def flag_to_code(config, condition_id, template_arg, args):
    return cg.new_Pvariable(condition_id, template_arg, config)


def payload_condition(value):
    if isinstance(value, dict):
        return cv.All(cv.Schema({
            cv.Optional("above"): float_payload,
            cv.Optional("below"): float_payload,
        }), cv.has_at_least_one_key("above", "below"), validate_range)(value)
    if isinstance(value, bool):
        return {"bool": value}
    if isinstance(value, (float, int)):
        return {"float": float_payload(value)}
    return {"string": string_payload(value)}


def validate_range(value):
    if "above" in value and "below" in value and value["above"] >= value["below"]:
        raise cv.Invalid("above must be smaller than below")
    return value


@automation.register_condition("thread_p2p_transporter.payload", PayloadCondition, payload_condition)
async def payload_to_code(config, condition_id, template_arg, args):
    var = cg.new_Pvariable(condition_id, template_arg)
    for key in ("bool", "float", "string", "above", "below"):
        if key in config:
            cg.add(getattr(var, "set_"+key)(config[key]))
    return var


def publish_schema(value):
    from esphome.components import sensor, binary_sensor, text_sensor
    return cv.All(cv.Schema({
        cv.Optional("sensor_id"): cv.use_id(sensor.Sensor),
        cv.Optional("binary_sensor_id"): cv.use_id(binary_sensor.BinarySensor),
        cv.Optional("text_sensor_id"): cv.use_id(text_sensor.TextSensor),
    }), cv.has_exactly_one_key("sensor_id", "binary_sensor_id", "text_sensor_id"))(value)


@automation.register_action("thread_p2p_transporter.publish", PublishAction, publish_schema, synchronous=True)
async def publish_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    for key in ("sensor_id", "binary_sensor_id", "text_sensor_id"):
        if key in config:
            cg.add(getattr(var, "set_"+key)(await cg.get_variable(config[key])))
    return var
