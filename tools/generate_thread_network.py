"""Generate pre-shared Thread credentials using the OS cryptographic RNG."""
import argparse
from datetime import datetime
import ipaddress
from pathlib import Path
import secrets
import sys


def generate_network(channel=None, name=None):
    if channel is not None and (isinstance(channel, bool) or not 11 <= channel <= 26):
        raise ValueError("channel must be between 11 and 26")
    if name is None:
        name = "DIY-" + secrets.token_hex(4)
    if not 1 <= len(name.encode("utf-8")) <= 16 or any(ord(c) < 32 for c in name):
        raise ValueError("network name must contain 1..16 UTF-8 bytes and no control characters")
    # Locally assigned ULA /64: fd + random global ID and subnet ID.
    prefix = ipaddress.IPv6Network((int.from_bytes(b"\xfd" + secrets.token_bytes(7) + bytes(8), "big"), 64))
    return {
        "channel": channel if channel is not None else 11 + secrets.randbelow(16),
        "network_name": name,
        "network_key": secrets.token_hex(16),
        "pan_id": secrets.randbelow(0xFFFF),  # 0xffff is the broadcast PAN ID.
        "ext_pan_id": secrets.token_hex(8),
        "pskc": secrets.token_hex(16),
        "mesh_local_prefix": str(prefix),
        "force_dataset": True,
    }


def render_network(config):
    # JSON string quoting is also valid YAML, including non-ASCII network names.
    import json
    return "\n".join([
        "# Paste this block under thread_br; use the SAME values on all nodes.",
        "# Keep this file private: network_key and pskc are network credentials.",
        "thread_network_config:",
        f'  channel: {config["channel"]}',
        f'  network_name: {json.dumps(config["network_name"], ensure_ascii=False)}',
        f'  network_key: 0x{config["network_key"]}',
        f'  pan_id: 0x{config["pan_id"]:04x}',
        f'  ext_pan_id: 0x{config["ext_pan_id"]}',
        f'  pskc: 0x{config["pskc"]}',
        f'  mesh_local_prefix: {config["mesh_local_prefix"]}',
        "  force_dataset: true", "",
    ])


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--channel", type=int, help="11..26; omitted means random")
    parser.add_argument("--network-name", help="1..16 UTF-8 bytes; omitted means random")
    output = parser.add_mutually_exclusive_group()
    output.add_argument("--output", type=Path, help="Save to this new file (existing files are never overwritten)")
    output.add_argument("--stdout-only", action="store_true")
    args = parser.parse_args(argv)
    try:
        content = render_network(generate_network(args.channel, args.network_name))
        if not args.stdout_only:
            target = args.output or Path(__file__).parent / "output" / (
                "thread_network_config-" + datetime.now().strftime("%Y%m%d-%H%M%S-") + secrets.token_hex(3) + ".yaml"
            )
            target.parent.mkdir(parents=True, exist_ok=True)
            with target.open("x", encoding="utf-8", newline="\n") as handle:
                handle.write(content)
            print(f"Saved: {target.resolve()}", file=sys.stderr)
        print(content, end="")
        return 0
    except (ValueError, OSError) as err:
        print(f"Error: {err}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
