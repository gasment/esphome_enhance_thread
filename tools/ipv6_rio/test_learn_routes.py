"""Offline protocol/lifetime checks; no sockets or system routes are touched."""
import ipaddress
import struct
import unittest
from unittest.mock import Mock
from types import SimpleNamespace

from learn_routes import checksum, parse_ra, RouteState, RouteWriter

SOURCE = "fe80::1234"
DEST = "ff02::1"
PREFIX = ipaddress.IPv6Network("fda1:d068:b97e:1::/64")


def packet(options):
    data = struct.pack("!BBHBBHII", 134, 0, 0, 64, 0, 0, 0, 0) + options
    pseudo = ipaddress.IPv6Address(SOURCE).packed + ipaddress.IPv6Address(DEST).packed
    pseudo += struct.pack("!I3xB", len(data), 58)
    return data[:2] + struct.pack("!H", checksum(pseudo + data)) + data[4:]


def rio(preference=0, lifetime=1800):
    return struct.pack("!BBBBI", 24, 2, 64, preference << 3, lifetime) + PREFIX.network_address.packed[:8]


class ProtocolTests(unittest.TestCase):
    def test_valid_rio_even_when_default_router_lifetime_is_zero(self):
        self.assertEqual(parse_ra(packet(rio()), SOURCE, DEST, 255), [(PREFIX, 0, 1800)])

    def test_reject_invalid_ra(self):
        good = packet(rio())
        for data, source, hop in [(good, SOURCE, 64), (good, "fd00::1", 255),
                                  (good[:-1], SOURCE, 255),
                                  (packet(b"\x01\x00"), SOURCE, 255)]:
            with self.assertRaises(ValueError):
                parse_ra(data, source, DEST, hop)

    def test_reserved_preference_ignored(self):
        self.assertEqual(parse_ra(packet(rio(2)), SOURCE, DEST, 255), [])

    def test_withdraw_expiry_and_other_router(self):
        state = RouteState()
        state.update(SOURCE, [(PREFIX, 1, 30)], 100)
        state.update("fe80::5678", [(PREFIX, 0, 100)], 100)
        self.assertEqual(state.selected(101)[str(PREFIX)][1], SOURCE)
        state.update(SOURCE, [(PREFIX, 1, 0)], 102)
        self.assertEqual(state.selected(103)[str(PREFIX)][1], "fe80::5678")
        self.assertEqual(state.selected(200), {})

    def test_refresh_extends_and_absent_option_does_not_withdraw(self):
        state = RouteState()
        state.update(SOURCE, [(PREFIX, 0, 30)], 100)
        state.update(SOURCE, [], 110)
        state.update(SOURCE, [(PREFIX, 0, 30)], 120)
        self.assertEqual(state.selected(140)[str(PREFIX)][2], 150)

    def test_writer_uses_expiring_routes_and_does_not_replace_foreign_route(self):
        writer = RouteWriter(SimpleNamespace(interface="ovs_eth2", table="main"))
        writer.command = Mock(side_effect=RuntimeError("File exists"))
        desired = {str(PREFIX): (3072, SOURCE, 200)}
        writer.sync(desired, 100)
        self.assertEqual(writer.installed, {})
        self.assertEqual(writer.command.call_args[0][0][0], "add")
        writer.command = Mock()
        writer.sync(desired, 100)
        self.assertIn("expires", writer.command.call_args[0][0])
        writer.sync({str(PREFIX): (3072, SOURCE, 230)}, 130)
        self.assertEqual(writer.command.call_args[0][0][0], "replace")
        writer.sync({}, 231)
        self.assertEqual(writer.command.call_args[0][0][0], "del")


if __name__ == "__main__":
    unittest.main()
