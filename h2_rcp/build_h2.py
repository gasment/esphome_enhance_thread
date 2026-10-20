"""Build a customized H2 RCP using ESPHome's ESP-IDF and YAML IO/LED settings."""
import argparse
from dataclasses import dataclass
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
FORMAT_VERSION = 5
FIRMWARE = ROOT / "h2_rcp/firmware"
DEFAULT_CONFIG = ROOT / "h2_rcp/h2.yaml"


def mapping(value, allowed, label):
    if not isinstance(value, dict):
        raise ValueError(f"{label} must be a YAML mapping")
    unknown = set(value) - set(allowed)
    if unknown:
        raise ValueError(f"Unknown {label} option(s): {', '.join(map(str, unknown))}")
    missing = set(allowed) - set(value)
    if missing:
        raise ValueError(f"Missing {label} option(s): {', '.join(sorted(missing))}")
    return value


def gpio(value):
    if isinstance(value, bool):
        raise ValueError("GPIO must be an integer or GPIO<number>")
    if isinstance(value, str) and re.fullmatch(r"GPIO\d+", value, re.I):
        value = int(value[4:])
    # Board/module accessible digital pads. 6/7 and 15..21 are reserved for flash.
    if not isinstance(value, int) or value not in {*range(0, 6), *range(8, 15), *range(22, 28)}:
        raise ValueError(f"GPIO {value!r} is not an available H2 module IO")
    if value == 9:
        raise ValueError("H2 GPIO9 is reserved for BOOT coordination with S3")
    if value in (26, 27):
        raise ValueError("H2 GPIO26/27 are reserved for native USB Serial/JTAG logging")
    return value


def load_config(path):
    try:
        import yaml
    except ImportError as err:
        raise ValueError("Activate your ESPHome Python environment (PyYAML is required)") from err
    try:
        raw = yaml.safe_load(Path(path).read_text(encoding="utf-8"))
    except yaml.YAMLError as err:
        raise ValueError(f"Invalid H2 YAML: {err}") from err
    if isinstance(raw, dict):
        raw.setdefault("leds", {})
    data = mapping(raw, ("name", "esp_idf_version", "uart", "coexistence", "leds"), "h2")
    if not isinstance(data["name"], str) or not re.fullmatch(r"[a-z0-9][a-z0-9_-]{0,47}", data["name"]):
        raise ValueError("name must be 1..48 lowercase letters, digits, '_' or '-'")
    if not re.fullmatch(r"5\.5\.\d+", str(data["esp_idf_version"])):
        raise ValueError("This tool requires a pinned ESP-IDF 5.5.x version")
    result = {"name": data["name"], "esp_idf_version": str(data["esp_idf_version"])}
    for group, keys in (("uart", ("tx_pin", "rx_pin")),
                        ("coexistence", ("request_pin", "priority_pin", "grant_pin", "tx_line_pin"))):
        values = mapping(data[group], keys, group)
        result[group] = {key: gpio(values[key]) for key in keys}
    pins = [pin for group in ("uart", "coexistence") for pin in result[group].values()]
    if len(pins) != len(set(pins)):
        raise ValueError("All UART and coexistence GPIOs must be distinct")
    if any(pin > 25 for pin in result["uart"].values()):
        raise ValueError("The official ot_rcp UART pin configuration supports GPIO0..25")
    defaults = {
        "activity": {"pin": "GPIO22", "inverted": False, "enabled_on_boot": True,
                     "blink_interval_ms": 100, "hold_ms": 200},
        "connection": {"pin": "GPIO25", "inverted": False, "enabled_on_boot": True,
                       "blink_interval_ms": 500, "timeout_ms": 20000},
    }
    leds = data["leds"]
    if not isinstance(leds, dict) or set(leds) - set(defaults):
        raise ValueError("leds supports only activity and connection mappings")
    result["leds"] = {}
    for kind, values in defaults.items():
        supplied = leds.get(kind, {})
        if not isinstance(supplied, dict) or set(supplied) - set(values):
            raise ValueError(f"Unknown leds.{kind} option or invalid mapping")
        values = {**values, **supplied}
        values["pin"] = gpio(values["pin"])
        for key in ("inverted", "enabled_on_boot"):
            if not isinstance(values[key], bool):
                raise ValueError(f"leds.{kind}.{key} must be true or false")
        for key in set(values) - {"pin", "inverted", "enabled_on_boot"}:
            value = values[key]
            minimum = 10000 if key == "timeout_ms" else 20
            if isinstance(value, bool) or not isinstance(value, int) or not minimum <= value <= 60000:
                raise ValueError(f"leds.{kind}.{key} must be {minimum}..60000 milliseconds")
        if kind == "activity" and values["hold_ms"] < values["blink_interval_ms"]:
            raise ValueError("leds.activity.hold_ms must be at least blink_interval_ms")
        result["leds"][kind] = values
        pins.append(values["pin"])
    if len(pins) != len(set(pins)):
        raise ValueError("UART, coexistence and LED GPIOs must all be distinct")
    return result


def settings(config):
    coex = config["coexistence"]
    return {
        "CONFIG_OPENTHREAD_RADIO": True,
        "CONFIG_OPENTHREAD_RCP_UART": True,
        "CONFIG_OPENTHREAD_NCP_VENDOR_HOOK": True,
        "CONFIG_OPENTHREAD_UART_PIN_MANUAL": True,
        "CONFIG_OPENTHREAD_UART_RX_PIN": config["uart"]["rx_pin"],
        "CONFIG_OPENTHREAD_UART_TX_PIN": config["uart"]["tx_pin"],
        "CONFIG_ESP_COEX_EXTERNAL_COEXIST_ENABLE": True,
        "CONFIG_EXTERNAL_COEX_WORK_MODE_FOLLOWER": True,
        "CONFIG_EXTERNAL_COEX_WIRE_TYPE": 3,
        "CONFIG_EXTERNAL_COEX_REQUEST_PIN": coex["request_pin"],
        "CONFIG_EXTERNAL_COEX_PRIORITY_PIN": coex["priority_pin"],
        "CONFIG_EXTERNAL_COEX_GRANT_PIN": coex["grant_pin"],
        "CONFIG_EXTERNAL_COEX_TX_LINE_PIN": coex["tx_line_pin"],
        "CONFIG_OPENTHREAD_RCP_SPINEL_CONSOLE": False,
        "CONFIG_OPENTHREAD_CLI": False,
        "CONFIG_OPENTHREAD_CONSOLE_ENABLE": False,
        "CONFIG_OPENTHREAD_COMMISSIONER": False,
        "CONFIG_OPENTHREAD_JOINER": False,
        "CONFIG_OPENTHREAD_BORDER_ROUTER": False,
        "CONFIG_LOG_DEFAULT_LEVEL_NONE": False,
        "CONFIG_LOG_DEFAULT_LEVEL_INFO": True,
        "CONFIG_LOG_DEFAULT_LEVEL": 3,
        "CONFIG_BOOTLOADER_LOG_LEVEL_INFO": True,
        "CONFIG_ESP_CONSOLE_NONE": False,
        "CONFIG_ESP_CONSOLE_UART_DEFAULT": False,
        "CONFIG_ESP_CONSOLE_UART_CUSTOM": False,
        "CONFIG_ESP_CONSOLE_UART": False,
        "CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG": True,
        "CONFIG_ESP_CONSOLE_SECONDARY_NONE": True,
    }


def render_defaults(config):
    return "# Generated from h2.yaml; UART0 / 460800 / four-wire follower.\n" + "".join(
        f"{key}={'y' if value is True else 'n' if value is False else value}\n"
        for key, value in settings(config).items()
    )


@dataclass
class Environment:
    framework: Path
    python: Path | None
    env: dict


def framework_version(framework):
    version_file = framework / "version.txt"
    if version_file.exists():
        return version_file.read_text().strip().removeprefix("v")
    cmake = (framework / "tools/cmake/version.cmake").read_text()
    return ".".join(re.search(rf"set\(IDF_VERSION_{part}\s+(\d+)\)", cmake).group(1)
                    for part in ("MAJOR", "MINOR", "PATCH"))


def resolve_environment(config, args):
    env = os.environ.copy()
    env.pop("PYTHONPATH", None)
    explicit = args.idf_path or env.get("IDF_PATH")
    if explicit:
        framework = Path(explicit).resolve()
        python = Path(args.idf_python).resolve() if args.idf_python else None
        if python is None and env.get("IDF_PYTHON_ENV_PATH"):
            base = Path(env["IDF_PYTHON_ENV_PATH"])
            python = base / ("Scripts/python.exe" if os.name == "nt" else "bin/python")
        if not args.prepare_only and (python is None or not python.is_file()):
            raise ValueError("Export the ESPHome/IDF environment, supply --idf-python, or unset IDF_PATH for automatic setup")
        env["IDF_PATH"] = str(framework)
        if python:
            env["PATH"] = str(python.parent) + os.pathsep + env.get("PATH", "")
    else:
        try:
            from esphome.espidf.framework import check_esp_idf_install, get_framework_env
        except ImportError as err:
            raise ValueError("Run this script with the ESPHome 2026.9+ Python environment") from err
        # ESPHome manages both the framework and IDF's isolated Python/toolchain.
        # Requesting H2 installs the RISC-V toolchain if only S3 was built before.
        framework, python_env = check_esp_idf_install(config["esp_idf_version"], targets=["esp32h2"])
        framework = Path(framework)
        env = get_framework_env(framework, python_env, env)
        python = Path(python_env) / ("Scripts/python.exe" if os.name == "nt" else "bin/python")
    actual = framework_version(framework)
    if actual != config["esp_idf_version"]:
        raise ValueError(f"IDF version mismatch: YAML requests {config['esp_idf_version']}, environment contains {actual}")
    if not (framework / "examples/openthread/ot_rcp/main/esp_ot_rcp.c").is_file():
        raise ValueError("ESP-IDF installation does not contain the official ot_rcp example")
    env["IDF_TARGET"] = "esp32h2"
    env["PYTHONUTF8"] = "1"
    return Environment(framework, python, env)


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def write_changed(path, content):
    path.parent.mkdir(parents=True, exist_ok=True)
    if not path.exists() or path.read_text(encoding="utf-8") != content:
        path.write_text(content, encoding="utf-8", newline="\n")


def render_led_config(config):
    lines = ["// Generated from h2.yaml", "#pragma once"]
    for kind, values in config["leds"].items():
        for key, value in values.items():
            lines.append(f"#define H2_LED_{kind.upper()}_{key.upper()} {int(value)}")
    return "\n".join(lines) + "\n"


def customize_vendor(source):
    # Patch only the generated copy. Keep Espressif's other vendor handlers.
    anchor = "otError NcpBase::VendorCommandHandler(uint8_t aHeader, unsigned int aCommand)\n{"
    if source.count(anchor) != 1:
        raise ValueError("Unsupported ESP-IDF NCP vendor hook layout")
    handler = (FIRMWARE / "vendor_command.inc").read_text(encoding="utf-8")
    return '#include "h2_leds.h"\n' + source.replace(anchor, anchor + "\n" + handler, 1)


def customize_rcp_main(content):
    entry = "void app_main(void)\n{"
    storage = "ESP_ERROR_CHECK(nvs_flash_init());"
    start = "ESP_ERROR_CHECK(esp_openthread_start(&config));"
    if any(content.count(anchor) != 1 for anchor in (entry, storage, start)):
        raise ValueError("Unsupported ESP-IDF RCP app_main/NVS/start layout")
    content = '#include "h2_leds.h"\n#include "h2_usb_log.h"\n' + content
    content = content.replace(entry, entry + "\n    h2_usb_log_init();", 1)
    content = content.replace(storage, storage + "\n    h2_leds_init();", 1)
    return content.replace(start, start + "\n    h2_leds_start_reporting();", 1)


def prepare_project(config, environment, build_root):
    idf = environment.framework
    example = idf / "examples/openthread/ot_rcp"
    common = idf / "examples/openthread/ot_common_components/ot_examples_common"
    copies = {
        "CMakeLists.txt": example / "CMakeLists.txt",
        "partitions.csv": example / "partitions.csv",
        "sdkconfig.defaults": example / "sdkconfig.defaults",
        "main/esp_ot_rcp.c": example / "main/esp_ot_rcp.c",
        "main/esp_ot_config.h": example / "main/esp_ot_config.h",
        "main/Kconfig.projbuild": example / "main/Kconfig.projbuild",
        "main/esp_openthread_ncp.cpp": idf / "components/openthread/src/ncp/esp_openthread_ncp.cpp",
        "components/ot_examples_common/ot_external_coexist.c": common / "ot_external_coexist.c",
        "components/ot_examples_common/include/ot_examples_common.h": common / "include/ot_examples_common.h",
        "components/ot_examples_common/Kconfig.projbuild": common / "Kconfig.projbuild",
    }
    header = copies["main/esp_ot_config.h"].read_text(encoding="utf-8")
    if not re.search(r"\.baud_rate\s*=\s*460800\b", header):
        raise ValueError("Official RCP UART speed changed; expected 460800")
    provenance = {str(p.relative_to(idf)).replace("\\", "/"): digest(p) for p in copies.values()}
    custom = {p.name: digest(p) for p in sorted(FIRMWARE.iterdir()) if p.is_file()}
    identity = {"config": config, "upstream_sha256": provenance,
                "custom_sha256": custom, "tool_sha256": digest(__file__), "tool_format": FORMAT_VERSION}
    fingerprint = hashlib.sha256(json.dumps(identity, sort_keys=True).encode()).hexdigest()[:12]
    project = Path(build_root).resolve() / f'{config["name"]}-{config["esp_idf_version"]}-{fingerprint}'
    for name, source in copies.items():
        content = source.read_text(encoding="utf-8")
        if name == "main/esp_openthread_ncp.cpp":
            content = customize_vendor(content)
        elif name == "main/esp_ot_rcp.c":
            content = customize_rcp_main(content)
        write_changed(project / name, content)
    for name in ("h2_leds.c", "h2_leds.h", "h2_led_report.cpp", "h2_usb_log.c", "h2_usb_log.h"):
        write_changed(project / "main" / name, (FIRMWARE / name).read_text(encoding="utf-8"))
    write_changed(project / "main/h2_led_config.h", render_led_config(config))
    # Keep the official RCP application and GPIO coexistence implementation.
    # Drop example-only CLI/debug/iperf dependencies: no console is built here.
    write_changed(project / "main/CMakeLists.txt", (FIRMWARE / "CMakeLists.txt").read_text(encoding="utf-8"))
    write_changed(project / "main/idf_component.yml", '''dependencies:
  ot_examples_common:
    path: ../components/ot_examples_common
''')
    write_changed(project / "components/ot_examples_common/CMakeLists.txt", '''idf_component_register(
    SRCS "ot_external_coexist.c" INCLUDE_DIRS "include" REQUIRES esp_coex openthread
)
''')
    write_changed(project / "sdkconfig.h2.defaults", render_defaults(config))
    write_changed(project / "source-manifest.json", json.dumps(identity, indent=2) + "\n")
    license_file = idf / "LICENSE"
    if license_file.exists():
        write_changed(project / "LICENSE.esp-idf", license_file.read_text(encoding="utf-8"))
    return project


def run(command, environment, cwd):
    subprocess.run(list(map(str, command)), cwd=cwd, env=environment.env, check=True)


def verify_sdkconfig(project, config):
    text = (project / "sdkconfig").read_text(encoding="utf-8")
    for key, value in settings(config).items():
        wanted = "y" if value is True else "n" if value is False else str(value)
        valid = f"{key}={wanted}" in text.splitlines()
        if wanted == "n":
            # Kconfig omits invisible symbols entirely in RADIO-only builds.
            valid |= not any(line.startswith(key + "=") for line in text.splitlines())
        if not valid:
            raise ValueError(f"Generated sdkconfig does not match YAML: {key} should be {wanted}")


def export_firmware(project, config, environment, output_root):
    build = project / "build"
    flash = json.loads((build / "flasher_args.json").read_text(encoding="utf-8"))
    destination = Path(output_root).resolve() / project.name
    destination.mkdir(parents=True, exist_ok=True)
    settings_ = flash["flash_settings"]
    command = [environment.python, "-m", "esptool", "--chip", "esp32h2", "merge_bin",
               "-o", destination / "firmware.factory.bin"]
    for key in ("flash_mode", "flash_freq", "flash_size"):
        command += ["--" + key, settings_[key]]
    files = {}
    for offset, name in sorted(flash["flash_files"].items(), key=lambda item: int(item[0], 0)):
        source = build / name
        command += [offset, source]
        files[offset] = {"file": source.name, "sha256": digest(source)}
        shutil.copyfile(source, destination / source.name)
    run(command, environment, build)
    shutil.copyfile(project / "sdkconfig", destination / "sdkconfig")
    shutil.copyfile(project / "source-manifest.json", destination / "source-manifest.json")
    shutil.copyfile(project / "main/h2_led_config.h", destination / "h2_led_config.h")
    release = {"config": config, "chip": "esp32h2", "uart_port": 0, "baud_rate": 460800,
               "logging": {"transport": "USB Serial/JTAG", "level": "INFO"},
               "led_control_protocol": "H2LE/2 Spinel command/status property 15360",
               "coexistence": "four-wire follower", "flash_files": files,
               "factory_sha256": digest(destination / "firmware.factory.bin")}
    write_changed(destination / "firmware-manifest.json", json.dumps(release, indent=2) + "\n")
    return destination


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("config", nargs="?", type=Path, default=DEFAULT_CONFIG)
    parser.add_argument("--check-config", action="store_true", help="Only validate YAML, no environment setup")
    parser.add_argument("--prepare-only", action="store_true", help="Prepare the official source, without compiling")
    parser.add_argument("--idf-path", type=Path, help="Use an existing ESPHome IDF installation/exported environment")
    parser.add_argument("--idf-python", type=Path, help="IDF's Python executable, for an existing environment")
    parser.add_argument("--build-root", type=Path, default=ROOT / "h2_rcp/.cache/build")
    parser.add_argument("--output-root", type=Path, default=ROOT / "h2_rcp/output")
    parser.add_argument("--flash", action="store_true", help="Also flash the H2 (requires --port)")
    parser.add_argument("--port", help="Serial port for explicit --flash")
    args = parser.parse_args(argv)
    try:
        if args.flash and (not args.port or args.check_config or args.prepare_only):
            raise ValueError("--flash requires --port and a complete build")
        if args.port and not args.flash:
            raise ValueError("--port is only used together with --flash")
        config = load_config(args.config)
        if args.check_config:
            print("H2 configuration valid: UART0 / 460800 / four-wire follower / activity and connection LEDs")
            return 0
        environment = resolve_environment(config, args)
        project = prepare_project(config, environment, args.build_root)
        print(f"IDF: {environment.framework}\nProject: {project}", flush=True)
        if args.prepare_only:
            return 0
        idf_py = environment.framework / "tools/idf.py"
        run([environment.python, idf_py, "-B", "build", "-D", "IDF_TARGET=esp32h2", "-D",
             "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.h2.defaults", "build"], environment, project)
        verify_sdkconfig(project, config)
        output = export_firmware(project, config, environment, args.output_root)
        print(f"Firmware and SHA256 manifest: {output}", flush=True)
        if args.flash:
            run([environment.python, idf_py, "-B", "build", "-p", args.port, "flash"], environment, project)
        return 0
    except (OSError, ValueError, subprocess.CalledProcessError) as err:
        print(f"Error: {err}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
