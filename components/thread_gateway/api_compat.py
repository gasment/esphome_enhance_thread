"""Build-local Native API adaptation for a runtime-selected sub-device registry.

Never modify the installed ESPHome package. Skip unused reserved DeviceInfo slots
in both protobuf encoding and sizing, otherwise each empty slot becomes a phantom
device in HA. Strict matching fails early if upstream serialization changes.
"""
from pathlib import Path
from esphome import core
from esphome.components import api
from esphome.components.esp32 import add_extra_build_file

_DEVICE_LOOP = "  for (const auto &it : this->devices) {\n"
_FILTERED_LOOP = _DEVICE_LOOP + "    if (it.device_id == 0) continue;  // Thread gateway: unused device slot\n"


def patch_api_source(source):
    if source.count(_DEVICE_LOOP) != 2:
        raise ValueError("Unsupported ESPHome Native API device encoding layout; expected encode and size device loops")
    if "DeviceInfoResponse::encode(" not in source or "DeviceInfoResponse::calculate_size()" not in source:
        raise ValueError("Unsupported ESPHome Native API DeviceInfoResponse")
    return patch_sleep_field_guards(source.replace(_DEVICE_LOOP, _FILTERED_LOOP), 2)


def patch_sleep_field_guards(source, expected):
    """Expose only the protocol flag, without enabling actual deep-sleep support."""
    import re
    pattern = r"#ifdef USE_DEEP_SLEEP\n([^\n]*\bhas_deep_sleep\b[^\n]*\n)#endif"
    result, count = re.subn(pattern,
        r"#if defined(USE_DEEP_SLEEP) || defined(USE_THREAD_GATEWAY)\n\1#endif", source)
    if count != expected:
        raise ValueError("Unsupported Native API has_deep_sleep field guards")
    return result


def api_header_source():
    return (Path(api.__file__).parent / "api_pb2.h").read_text(encoding="utf-8")


def patch_api_header(source):
    return patch_sleep_field_guards(source, 1)


def api_source():
    return (Path(api.__file__).parent / "api_pb2.cpp").read_text(encoding="utf-8")


def validate_api_support():
    try:
        patch_api_source(api_source())
        patch_api_header(api_header_source())
        patch_connection_source(connection_source())
    except (ValueError, OSError) as err:
        raise ValueError(f"thread_gateway sub-devices require a supported ESPHome Native API: {err}") from err


def install_api_overlay():
    # User-generated intermediate files belong exclusively under project temp.
    path = Path(__file__).resolve().parents[2] / "temp/thread_gateway_api/api_pb2.cpp"
    patched = patch_api_source(api_source())
    path.parent.mkdir(parents=True, exist_ok=True)
    if not path.exists() or path.read_text(encoding="utf-8") != patched:
        path.write_text(patched, encoding="utf-8", newline="\n")
    header_path = path.parent / "api_pb2.h"
    header_path.write_text(patch_api_header(api_header_source()), encoding="utf-8", newline="\n")
    if not add_extra_build_file("src/esphome/components/api/api_pb2.h", header_path):
        raise core.EsphomeError("Another component already overrides Native API api_pb2.h")
    connection_path = path.parent / "api_connection.cpp"
    connection_path.write_text(patch_connection_source(connection_source()), encoding="utf-8", newline="\n")
    if not add_extra_build_file("src/esphome/components/api/api_connection.cpp", connection_path):
        raise core.EsphomeError("Another component already overrides Native API api_connection.cpp")
    target = "src/esphome/components/api/api_pb2.cpp"
    if not add_extra_build_file(target, path):
        raise core.EsphomeError("Another component already overrides Native API api_pb2.cpp")


def connection_source():
    return (Path(api.__file__).parent / "api_connection.cpp").read_text(encoding="utf-8")


def patch_connection_source(source):
    """Keep gateway planned restarts available; route lights without optimistic local execution."""
    sleep_marker = "#ifdef USE_DEEP_SLEEP\n  resp.has_deep_sleep = deep_sleep::global_has_deep_sleep;\n#endif"
    light_marker = "void APIConnection::on_light_command_request(const LightCommandRequest &msg) {\n"
    if source.count(sleep_marker) != 1 or source.count(light_marker) != 1:
        raise ValueError("Unsupported Native API connection layout for gateway restart/light adaptation")
    source = source.replace(sleep_marker, sleep_marker + "\n  resp.has_deep_sleep = true;  // Gateway clean restarts retain HA state; never enters deep sleep")
    code = "  auto *gateway_command = cJSON_CreateObject();\n"
    fields = ["state", "brightness", "color_mode", "color_brightness", "white", "color_temperature", "cold_white", "warm_white", "transition_length", "flash_length", "effect"]
    for field in fields:
        add = "cJSON_AddBoolToObject" if field == "state" else "cJSON_AddStringToObject" if field == "effect" else "cJSON_AddNumberToObject"
        value = f"msg.{field}.c_str()" if field == "effect" else f"static_cast<int>(msg.{field})" if field == "color_mode" else f"msg.{field}"
        code += f'  if (msg.has_{field}) {add}(gateway_command, "{field}", {value});\n'
    for field in ("red", "green", "blue"):
        code += f'  if (msg.has_rgb) cJSON_AddNumberToObject(gateway_command, "{field}", msg.{field});\n'
    code += "  const bool routed = thread_entity_bridge::route_light(msg.key, msg.device_id, gateway_command);\n  cJSON_Delete(gateway_command);\n  if (routed) return;\n"
    import re
    # Initial-state iterator treats a successful no-op as consumed; no defaults or
    # missing_state packets should overwrite HA's retained values after a restart.
    pattern = r"(bool APIConnection::send_(?:binary_sensor|cover|fan|light|sensor|switch|text_sensor|climate|number|select|text|lock|valve)_state\([^)]*?\*([a-zA-Z_]+)\) \{\n)"
    source, count = re.subn(pattern, lambda match: match[1] + f"  if (!thread_entity_bridge::remote_state_ready({match[2]})) return true;\n", source)
    if count != 13:
        raise ValueError(f"Unsupported Native API state send layout: expected 13 handlers, got {count}")
    replacements = {
        "msg.icon = StringRef(entity->get_icon_to(icon_buf));": "msg.icon = thread_entity_bridge::remote_metadata(entity, 2, StringRef(entity->get_icon_to(icon_buf)));",
        "device_class_field = StringRef(entity->get_device_class_to(dc_buf));": "device_class_field = thread_entity_bridge::remote_metadata(entity, 0, StringRef(entity->get_device_class_to(dc_buf)));",
        "msg.unit_of_measurement = sensor->get_unit_of_measurement_ref();": "msg.unit_of_measurement = thread_entity_bridge::remote_metadata(sensor, 1, sensor->get_unit_of_measurement_ref());",
        "msg.unit_of_measurement = number->get_unit_of_measurement_ref();": "msg.unit_of_measurement = thread_entity_bridge::remote_metadata(number, 1, number->get_unit_of_measurement_ref());",
    }
    for before, after in replacements.items():
        if source.count(before) != 1:
            raise ValueError("Unsupported Native API metadata encoding layout")
        source = source.replace(before, after)
    source = source.replace(light_marker, light_marker + code)
    return '#include "esphome/components/thread_entity_bridge/entity_bridge.h"\n' + source
