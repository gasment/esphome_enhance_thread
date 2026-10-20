"""Dataset validation shared by this fork's schema and regression checks."""
import ipaddress

import esphome.config_validation as cv


def fixed_hex(byte_length):
    def validate(value):
        if isinstance(value, bool):
            raise cv.Invalid("Expected a hexadecimal value, not a boolean")
        if isinstance(value, int):
            if not 0 <= value < 1 << (8 * byte_length):
                raise cv.Invalid(f"Expected a {byte_length}-byte unsigned value")
            return f"{value:0{byte_length * 2}x}"
        text = cv.string_strict(value).lower().removeprefix("0x")
        if len(text) != byte_length * 2 or any(c not in "0123456789abcdef" for c in text):
            raise cv.Invalid(f"Expected exactly {byte_length * 2} hexadecimal digits")
        return text

    return validate


def network_name(value):
    value = cv.string_strict(value)
    if not 1 <= len(value.encode("utf-8")) <= 16 or "\0" in value:
        raise cv.Invalid("Thread network_name must be 1..16 UTF-8 bytes without NUL")
    return value


def mesh_local_prefix(value):
    try:
        prefix = ipaddress.IPv6Network(cv.string_strict(value), strict=True)
    except ValueError as error:
        raise cv.Invalid(str(error)) from error
    if prefix.prefixlen != 64 or prefix.network_address.packed[0] != 0xfd:
        raise cv.Invalid("mesh_local_prefix must be an fd00::/8 ULA /64")
    return str(prefix)
