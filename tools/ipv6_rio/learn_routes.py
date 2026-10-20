#!/usr/bin/env python3
"""Linux/DSM userspace RIO learner. Observe by default; --apply modifies routes."""
import argparse
import ipaddress
import logging
import math
import shutil
import signal
import socket
import struct
import subprocess
import sys
import time

LOG = logging.getLogger("dsm_ra")
PROTOCOL = "186"  # Reserved for this tool on the selected interface/table.
METRICS = {1: 2048, 0: 3072, 3: 4096}


def checksum(data):
    if len(data) % 2:
        data += b"\0"
    value = sum(struct.unpack("!%dH" % (len(data) // 2), data))
    while value >> 16:
        value = (value & 0xffff) + (value >> 16)
    return (~value) & 0xffff


def parse_ra(packet, source, destination, hop_limit):
    """Validate RA and return RIOs; malformed option framing rejects the RA."""
    source = ipaddress.IPv6Address(source)
    if not source.is_link_local or hop_limit != 255:
        raise ValueError("RA source must be link-local and hop limit must be 255")
    if len(packet) < 16 or packet[0] != 134 or packet[1] != 0:
        raise ValueError("invalid RA header")
    pseudo = source.packed + ipaddress.IPv6Address(destination).packed
    pseudo += struct.pack("!I3xB", len(packet), 58)
    if checksum(pseudo + packet):
        raise ValueError("invalid ICMPv6 checksum")
    routes = []
    offset = 16
    while offset < len(packet):
        if offset + 2 > len(packet):
            raise ValueError("truncated option header")
        kind, units = packet[offset:offset + 2]
        size = units * 8
        if not size or offset + size > len(packet):
            raise ValueError("invalid option length")
        option = packet[offset:offset + size]
        offset += size
        if kind != 24:
            continue
        if units not in (1, 2, 3):
            continue
        plen, flags, lifetime = struct.unpack("!BBI", option[2:8])
        preference = (flags >> 3) & 3
        if plen > 128 or plen > (size - 8) * 8 or preference == 2:
            continue
        address = ipaddress.IPv6Address(option[8:].ljust(16, b"\0"))
        prefix = ipaddress.IPv6Network((address, plen), strict=False)
        routes.append((prefix, preference, lifetime))
    return routes


class RouteState:
    def __init__(self):
        self.entries = {}

    def update(self, router, routes, now):
        for prefix, preference, lifetime in routes:
            key = (str(prefix), router)
            if lifetime == 0:
                self.entries.pop(key, None)
            else:
                expiry = math.inf if lifetime == 0xffffffff else now + lifetime
                self.entries[key] = (preference, expiry)

    def selected(self, now):
        self.entries = {k: v for k, v in self.entries.items() if v[1] > now}
        winners = {}
        for (prefix, router), (preference, expiry) in self.entries.items():
            candidate = (METRICS[preference], router, expiry)
            # Stable tie-break; a refresh alone does not switch gateways.
            if prefix not in winners or candidate[:2] < winners[prefix][:2]:
                winners[prefix] = candidate
        return winners


class RouteWriter:
    def __init__(self, args):
        self.args = args
        self.installed = {}

    def command(self, arguments):
        result = subprocess.run(
            [self.args.ip, "-6", "route"] + arguments,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, universal_newlines=True,
            timeout=5,
        )
        if result.returncode:
            raise RuntimeError(result.stderr.strip() or "ip route command failed")

    def delete(self, prefix, route):
        metric, router, _ = route
        self.command(["del", prefix, "via", router, "dev", self.args.interface,
                      "table", self.args.table, "proto", PROTOCOL, "metric", str(metric)])

    def sync(self, desired, now, force=False):
        for prefix, previous in list(self.installed.items()):
            new = desired.get(prefix)
            if new is None or new[:2] != previous[:2]:
                try:
                    self.delete(prefix, previous)
                    LOG.info("Removed %s via %s", prefix, previous[1])
                except (RuntimeError, subprocess.TimeoutExpired) as error:
                    if previous[2] > now:
                        LOG.warning("Remove %s: %s", prefix, error)
                        continue
                del self.installed[prefix]
        for prefix, route in desired.items():
            previous = self.installed.get(prefix)
            if previous == route and not force:
                continue
            if previous is not None and previous[:2] != route[:2]:
                continue  # Retry deletion before changing gateway/metric.
            metric, router, expiry = route
            remaining = 86400 if math.isinf(expiry) else max(1, math.ceil(expiry - now))
            # Infinite advertisements get a renewable one-day kernel lease.
            # This also limits stale routes if the process is killed.
            arguments = ["replace" if previous else "add", prefix,
                         "via", router, "dev", self.args.interface,
                         "table", self.args.table, "proto", PROTOCOL,
                         "metric", str(metric), "expires", str(min(remaining, 86400))]
            try:
                self.command(arguments)
                self.installed[prefix] = route
                LOG.info("Learned %s via %s lifetime=%ss metric=%s", prefix, router, remaining, metric)
            except (RuntimeError, subprocess.TimeoutExpired) as error:
                LOG.error("Install %s: %s", prefix, error)


def open_socket(interface):
    index = socket.if_nametoindex(interface)
    sock = socket.socket(socket.AF_INET6, socket.SOCK_RAW, socket.IPPROTO_ICMPV6)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_BINDTODEVICE, interface.encode() + b"\0")
    sock.setsockopt(socket.IPPROTO_IPV6, socket.IPV6_RECVPKTINFO, 1)
    sock.setsockopt(socket.IPPROTO_IPV6, socket.IPV6_RECVHOPLIMIT, 1)
    sock.setsockopt(socket.IPPROTO_IPV6, socket.IPV6_MULTICAST_IF, index)
    sock.setsockopt(socket.IPPROTO_IPV6, socket.IPV6_MULTICAST_HOPS, 255)
    sock.setsockopt(socket.IPPROTO_IPV6, socket.IPV6_UNICAST_HOPS, 255)
    # Linux ICMP6_FILTER: receive RA only; the kernel verifies ICMPv6 as well.
    mask = [0xffffffff] * 8
    mask[134 >> 5] &= ~(1 << (134 & 31))
    sock.setsockopt(socket.IPPROTO_ICMPV6, 1, struct.pack("=8I", *mask))
    sock.settimeout(1)
    return sock, index


def run(args):
    if sys.platform != "linux":
        raise RuntimeError("Run this tool on Linux/DSM, not on the Windows build PC")
    import fcntl
    lock = None
    if args.apply:
        lock = open("/var/run/thread-rio-%s.lock" % args.interface, "a")
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            LOG.info("Another learner already runs on %s", args.interface)
            return
    sock, index = open_socket(args.interface)
    writer = RouteWriter(args)
    state = RouteState()
    stop = [False]
    for signum in (signal.SIGTERM, signal.SIGINT):
        signal.signal(signum, lambda *_: stop.__setitem__(0, True))
    if args.apply:
        # Only the dedicated protocol on the selected table/interface is ours.
        writer.command(["flush", "table", args.table, "dev", args.interface, "proto", PROTOCOL])
    LOG.info("%s on %s; ULA /64 RIOs; routers=%s",
             "APPLY" if args.apply else "OBSERVE (no route changes)", args.interface,
             ",".join(args.router) or "any link-local RA source")
    next_rs = 0
    refresh = 0
    try:
        while not stop[0]:
            now = time.monotonic()
            if now >= next_rs:
                try:
                    sock.sendto(struct.pack("!BBHI", 133, 0, 0, 0), ("ff02::2", 0, 0, index))
                except OSError as error:
                    LOG.warning("Router solicitation: %s", error)
                next_rs = now + (60 if state.entries else 10)
            try:
                packet, ancillary, flags, peer = sock.recvmsg(65535, 256)
                source = str(ipaddress.IPv6Address(peer[0].split("%")[0]))
                destination, received_index, hop_limit = None, None, None
                for level, kind, data in ancillary:
                    if level == socket.IPPROTO_IPV6 and kind == socket.IPV6_PKTINFO:
                        destination = socket.inet_ntop(socket.AF_INET6, data[:16])
                        received_index = struct.unpack("=I", data[16:20])[0]
                    elif level == socket.IPPROTO_IPV6 and kind == socket.IPV6_HOPLIMIT:
                        hop_limit = struct.unpack("=i", data)[0]
                if flags & (socket.MSG_TRUNC | socket.MSG_CTRUNC) or received_index != index or destination is None:
                    continue
                if args.router and source not in args.router:
                    continue
                routes = parse_ra(packet, source, destination, hop_limit)
                allowed = []
                for prefix, preference, lifetime in routes:
                    if prefix.prefixlen == 64 and prefix.subnet_of(ipaddress.IPv6Network("fc00::/7")):
                        allowed.append((prefix, preference, lifetime))
                        LOG.info("RA %s: %s preference=%s lifetime=%s", source, prefix, preference, lifetime)
                state.update(source, allowed, time.monotonic())
            except socket.timeout:
                pass
            except ValueError as error:
                LOG.debug("Ignored RA: %s", error)
            now = time.monotonic()
            desired = state.selected(now)
            if args.apply:
                # Refresh capped kernel leases even for infinite/long RIOs.
                force = now >= refresh
                if force:
                    refresh = now + 3600
                writer.sync(desired, now, force)
    finally:
        sock.close()
        if args.apply:
            writer.command(["flush", "table", args.table, "dev", args.interface, "proto", PROTOCOL])
        if lock:
            lock.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--interface", default="ovs_eth2")
    parser.add_argument("--router", action="append", default=[], help="Optional allowed link-local RA source; repeatable")
    parser.add_argument("--table", default="main", help="IPv6 route table (default: main)")
    parser.add_argument("--ip", default=shutil.which("ip") or "/sbin/ip")
    parser.add_argument("--apply", action="store_true", help="Install routes; otherwise only observe")
    parser.add_argument("--debug", action="store_true")
    args = parser.parse_args()
    for i, router in enumerate(args.router):
        address = ipaddress.IPv6Address(router)
        if not address.is_link_local:
            parser.error("--router must be a link-local IPv6 address without a zone suffix")
        args.router[i] = str(address)
    logging.basicConfig(level=logging.DEBUG if args.debug else logging.INFO,
                        format="%(asctime)s %(levelname)s %(message)s")
    try:
        run(args)
    except (OSError, RuntimeError, subprocess.TimeoutExpired) as error:
        LOG.error("%s", error)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
