#!/usr/bin/env python3
#
# Copyright © 2026 libratbag contributors
# SPDX-License-Identifier: MIT
#
# A small HID++ device emulator for the portable (macOS) libratbag build.
#
# It emulates a Logitech G703, either plugged in by cable or paired to a
# Lightspeed receiver, well enough for libratbag's hidpp20 driver: root and
# feature set, device info and name, battery, adjustable DPI, report rate
# and onboard profiles (with flash memory, CRCs and ROM profiles). Point
# libratbag at it with RATBAG_EMU_SOCKET:
#
#   ./hidpp-emu.py --socket /tmp/emu.sock --mode receiver &
#   RATBAG_EMU_SOCKET=/tmp/emu.sock ratbag-probe -v
#   RATBAG_EMU_SOCKET=/tmp/emu.sock piper
#
# Writes to the emulated flash are kept for as long as the emulator runs,
# and with --state FILE across runs.
#
# The protocol is described in src/libratbag-hid-emu.c.

import argparse
import json
import os
import socketserver
import struct
import sys
import threading

REPORT_SHORT = 0x10
REPORT_LONG = 0x11
LONG_LEN = 20

# HID++ 2.0 error codes
ERR_INVALID_ARGUMENT = 0x02
ERR_OUT_OF_RANGE = 0x03
ERR_INVALID_FEATURE_INDEX = 0x06
ERR_INVALID_FUNCTION_ID = 0x07

# HID++ 1.0 error codes
ERR10_INVALID_SUBID = 0x01
ERR10_INVALID_VALUE = 0x03
ERR10_UNKNOWN_DEVICE = 0x08

# Report descriptor of a Logitech HID++ interface: short (0x10) and long
# (0x11) reports on vendor page 0xff00
HIDPP_DESCRIPTOR = bytes.fromhex(
    "06 00 ff 09 01 a1 01 85 10 75 08 95 06 15 00 26 ff 00 09 01 81 00 09 01 91 00 c0"
    "06 00 ff 09 02 a1 01 85 11 75 08 95 13 15 00 26 ff 00 09 02 81 00 09 02 91 00 c0"
)

# A plain boot mouse interface, no report IDs
MOUSE_DESCRIPTOR = bytes.fromhex(
    "05 01 09 02 a1 01 09 01 a1 00 05 09 19 01 29 05 15 00 25 01 95 05 75 01 81 02"
    "95 01 75 03 81 01 05 01 09 30 09 31 09 38 15 81 25 7f 75 08 95 03 81 06 c0 c0"
)


def crc_ccitt(data: bytes) -> int:
    crc = 0xFFFF
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) if crc & 0x8000 else (crc << 1)
            crc &= 0xFFFF
    return crc


def log(*args):
    if os.environ.get("HIDPP_EMU_QUIET"):
        return
    print("[emu]", *args, file=sys.stderr, flush=True)


class G703:
    """A HID++ 2.0 G703 with onboard profiles."""

    NAME = "G703 LIGHTSPEED Wireless Gaming Mouse"
    WPID = 0x4070
    USB_PID = 0xC087
    SECTOR_SIZE = 255
    PROFILE_COUNT = 5
    ROM_PROFILE_COUNT = 1
    BUTTON_COUNT = 6

    FEATURES = [
        0x0000,  # root
        0x0001,  # feature set
        0x0003,  # device information
        0x0005,  # device name
        0x1001,  # battery voltage
        0x2201,  # adjustable dpi
        0x8060,  # adjustable report rate
        0x8100,  # onboard profiles
    ]

    def __init__(self, state_file=None):
        self.lock = threading.Lock()
        self.state_file = state_file
        self.onboard_mode = 1
        self.current_profile = 0
        self.current_dpi_index = 1
        self.dpi = 1600
        self.report_rate_ms = 1
        self.flash = {}
        self.write = None  # (sector, offset, count, data)
        self.rom = {0x0101: self._rom_profile()}
        self._load()

    # --- persistence --------------------------------------------------------

    def _load(self):
        if not self.state_file or not os.path.exists(self.state_file):
            return
        with open(self.state_file) as f:
            state = json.load(f)
        self.flash = {int(k): bytearray.fromhex(v) for k, v in state["flash"].items()}
        self.current_profile = state.get("current_profile", 0)
        log(f"loaded {len(self.flash)} flash sectors from {self.state_file}")

    def _save(self):
        if not self.state_file:
            return
        state = {
            "flash": {str(k): v.hex() for k, v in self.flash.items()},
            "current_profile": self.current_profile,
        }
        with open(self.state_file, "w") as f:
            json.dump(state, f)

    # --- memory -------------------------------------------------------------

    def _rom_profile(self) -> bytes:
        p = bytearray(b"\xff" * self.SECTOR_SIZE)
        p[0] = 1  # report rate: 1 ms
        p[1] = 1  # default dpi index
        p[2] = 0  # shifted dpi index
        for i, dpi in enumerate([400, 800, 1600, 3200, 6400]):
            struct.pack_into("<H", p, 3 + 2 * i, dpi)
        p[13:16] = b"\x00\x00\xff"  # profile color
        p[16] = 0x00  # power mode
        p[17] = 0x00  # angle snapping
        p[18:28] = b"\xff" * 10
        struct.pack_into("<H", p, 28, 0xFFFF)
        struct.pack_into("<H", p, 30, 0xFFFF)
        # buttons: 4 bytes each. 0x80 0x01 = HID mouse, big endian mask
        buttons = [
            b"\x80\x01\x00\x01",  # left
            b"\x80\x01\x00\x02",  # right
            b"\x80\x01\x00\x04",  # middle
            b"\x80\x01\x00\x08",  # back
            b"\x80\x01\x00\x10",  # forward
            b"\x90\x05\x00\x00",  # special: cycle resolution up
        ]
        for i in range(16):
            b = buttons[i] if i < len(buttons) else b"\xff\xff\xff\xff"
            p[32 + 4 * i : 36 + 4 * i] = b
            p[96 + 4 * i : 100 + 4 * i] = b"\xff\xff\xff\xff"
        # name stays 0xff: "use the default name"
        struct.pack_into(
            ">H", p, self.SECTOR_SIZE - 2, crc_ccitt(p[: self.SECTOR_SIZE - 2])
        )
        return bytes(p)

    def _sector(self, sector):
        if sector in self.rom:
            return self.rom[sector]
        return self.flash.get(sector, b"\xff" * self.SECTOR_SIZE)

    # --- HID++ 2.0 ----------------------------------------------------------

    def feature_index(self, feature):
        try:
            return self.FEATURES.index(feature)
        except ValueError:
            return 0

    def handle(self, feature_index, function, params):
        """Returns reply parameters, or an int HID++ 2.0 error code."""
        with self.lock:
            if feature_index >= len(self.FEATURES):
                return ERR_INVALID_FEATURE_INDEX
            feature = self.FEATURES[feature_index]
            handler = getattr(self, f"f{feature:04x}", None)
            return handler(function, params)

    def f0000(self, fn, p):  # root
        if fn == 0:
            feature = struct.unpack(">H", p[0:2])[0]
            idx = self.feature_index(feature)
            return bytes([idx, 0, 0])
        if fn == 1:
            return bytes([4, 2, p[2]])
        return ERR_INVALID_FUNCTION_ID

    def f0001(self, fn, p):  # feature set
        if fn == 0:
            return bytes([len(self.FEATURES) - 1])
        if fn == 1:
            idx = p[0]
            if idx >= len(self.FEATURES):
                return ERR_OUT_OF_RANGE
            return struct.pack(">HBB", self.FEATURES[idx], 0, 0)
        return ERR_INVALID_FUNCTION_ID

    def f0003(self, fn, p):  # device information
        if fn == 0:
            # entity count, unit id, transport (eQuad + USB), model ids
            return (
                bytes([2])
                + b"\x12\x34\x56\x78"
                + b"\x00\x0c"
                + struct.pack(">HHH", self.WPID, self.USB_PID, 0)
            )
        if fn == 1:
            # firmware: type 0, "MPM" 17.01 build 0x0042
            return (
                bytes([0])
                + b"MPM"
                + bytes([0x17, 0x01])
                + struct.pack(">H", 0x42)
                + bytes([1, 0, 0])
            )
        return ERR_INVALID_FUNCTION_ID

    def f0005(self, fn, p):  # device name
        name = self.NAME.encode()
        if fn == 0:
            return bytes([len(name)])
        if fn == 1:
            return name[p[0] : p[0] + 16]
        return ERR_INVALID_FUNCTION_ID

    def f1001(self, fn, p):  # battery voltage
        if fn == 0:
            return struct.pack(">HB", 3950, 0x00)
        return ERR_INVALID_FUNCTION_ID

    def f2201(self, fn, p):  # adjustable dpi
        if fn == 0:
            return bytes([1])
        if fn == 1:
            # 100 .. 16000 in steps of 50
            return bytes([0]) + struct.pack(">HHH", 100, 0xE000 + 50, 16000)
        if fn == 2:
            return bytes([0]) + struct.pack(">HH", self.dpi, 1600)
        if fn == 3:
            self.dpi = struct.unpack(">H", p[1:3])[0]
            log(f"sensor dpi set to {self.dpi}")
            return bytes(p[:3])
        return ERR_INVALID_FUNCTION_ID

    def f8060(self, fn, p):  # adjustable report rate
        if fn == 0:
            return bytes([0b10001011])  # 1, 2, 4, 8 ms
        if fn == 1:
            return bytes([self.report_rate_ms])
        if fn == 2:
            self.report_rate_ms = p[0]
            return b""
        return ERR_INVALID_FUNCTION_ID

    def f8100(self, fn, p):  # onboard profiles
        if fn == 0x0:
            return struct.pack(
                ">BBBBBBBHBB5x",
                0x01,  # memory model G402
                0x03,  # profile format G900
                0x01,  # macro format
                self.PROFILE_COUNT,
                self.ROM_PROFILE_COUNT,
                self.BUTTON_COUNT,
                16,  # sector count
                self.SECTOR_SIZE,
                0x00,  # mechanical layout
                0x04,  # corded and wireless
            )
        if fn == 0x1:
            self.onboard_mode = p[0]
            return b""
        if fn == 0x2:
            return bytes([self.onboard_mode])
        if fn == 0x3:
            self.current_profile = max(p[1] - 1, 0)
            log(f"current profile set to {self.current_profile}")
            self._save()
            return b""
        if fn == 0x4:
            return bytes([0, self.current_profile])
        if fn == 0x5:
            sector, offset = struct.unpack(">HH", p[0:4])
            if offset + 16 > self.SECTOR_SIZE:
                return ERR_INVALID_ARGUMENT
            return bytes(self._sector(sector)[offset : offset + 16])
        if fn == 0x6:
            sector, offset, count = struct.unpack(">HHH", p[0:6])
            if sector in self.rom:
                return ERR_INVALID_ARGUMENT
            self.write = [sector, offset, count, bytearray()]
            return b""
        if fn == 0x7:
            if not self.write:
                return ERR_INVALID_ARGUMENT
            self.write[3] += bytes(p[0:16])
            return b""
        if fn == 0x8:
            if not self.write:
                return ERR_INVALID_ARGUMENT
            sector, offset, count, data = self.write
            buf = bytearray(self._sector(sector))
            buf[offset : offset + count] = data[:count]
            self.flash[sector] = buf
            self.write = None
            log(f"wrote {count} bytes to sector 0x{sector:04x}")
            self._save()
            return b""
        if fn == 0xB:
            return bytes([self.current_dpi_index])
        if fn == 0xC:
            self.current_dpi_index = p[0]
            return b""
        return ERR_INVALID_FUNCTION_ID


def hidpp20_reply(report, device_index, device):
    """Answers a HID++ 2.0 request to device, or None."""
    feature_index, address = report[2], report[3]
    function = address >> 4
    params = bytes(report[4:]) + bytes(16)
    result = device.handle(feature_index, function, params)
    if isinstance(result, int):
        reply = bytes([REPORT_LONG, device_index, 0xFF, feature_index, address, result])
    else:
        reply = bytes([REPORT_LONG, device_index, feature_index, address]) + result
    return reply.ljust(LONG_LEN, b"\x00")[:LONG_LEN]


class Endpoint:
    """One emulated HID interface (one hidraw node / one IOHIDDevice)."""

    def __init__(
        self, path, group, vid, pid, interface, name, descriptor, handler=None
    ):
        self.path = path
        self.group = group
        self.vid = vid
        self.pid = pid
        self.interface = interface
        self.name = name
        self.descriptor = descriptor
        self.handler = handler

    def line(self):
        return "\t".join(
            [
                self.path,
                self.group,
                "0003",
                f"{self.vid:04x}",
                f"{self.pid:04x}",
                str(self.interface),
                self.name,
                self.descriptor.hex(),
            ]
        )

    def output(self, report):
        """Handles an output report, returns the input reports to send."""
        if (
            self.handler
            and len(report) >= 4
            and report[0] in (REPORT_SHORT, REPORT_LONG)
        ):
            return self.handler(report)
        return []


class WiredG703:
    def __init__(self, mouse):
        self.mouse = mouse

    def __call__(self, report):
        # the wired device answers to 0xff, and to index 0 like most do
        if report[1] not in (0xFF, 0x00):
            return []
        return [hidpp20_reply(report, report[1], self.mouse)]


class LightspeedReceiver:
    """A receiver with the G703 paired in slot 1."""

    def __init__(self, mouse, online=True):
        self.slots = {1: mouse}
        self.online = online

    def __call__(self, report):
        device_index = report[1]
        if device_index == 0xFF:
            return [self.receiver_request(report)]
        mouse = self.slots.get(device_index)
        if not mouse or not self.online:
            return [self.error10(report, ERR10_UNKNOWN_DEVICE)]
        return [hidpp20_reply(report, device_index, mouse)]

    def error10(self, report, code):
        return bytes([REPORT_SHORT, report[1], 0x8F, report[2], report[3], code, 0])

    def receiver_request(self, report):
        sub_id, address = report[2], report[3]
        # get long register 0xb5: pairing information
        if sub_id == 0x83 and address == 0xB5:
            sub = report[4]
            slot = (sub & 0x0F) + 1
            if (sub & 0xF0) == 0x20 and slot in self.slots:
                mouse = self.slots[slot]
                data = (
                    bytes([sub, 0x00, 0x08])
                    + struct.pack(">H", mouse.WPID)
                    + bytes([0, 0, 0x02])
                )
                return (bytes([REPORT_LONG, 0xFF, 0x83, 0xB5]) + data).ljust(
                    LONG_LEN, b"\x00"
                )
            return self.error10(report, ERR10_INVALID_VALUE)
        return self.error10(report, ERR10_INVALID_SUBID)


class Server(socketserver.ThreadingUnixStreamServer):
    daemon_threads = True
    allow_reuse_address = True

    def __init__(self, path, endpoints):
        self.endpoints = {e.path: e for e in endpoints}
        super().__init__(path, Handler)


class Handler(socketserver.BaseRequestHandler):
    def handle(self):
        sock = self.request
        f = sock.makefile("rb")
        line = f.readline().decode().strip()
        if line == "LIST":
            out = "".join(e.line() + "\n" for e in self.server.endpoints.values())
            sock.sendall(out.encode())
            return
        if not line.startswith("OPEN "):
            return
        endpoint = self.server.endpoints.get(line[5:])
        if not endpoint:
            return
        log(f"open {endpoint.path}")
        while True:
            hdr = f.read(3)
            if len(hdr) < 3:
                break
            kind, length = hdr[0], hdr[1] | hdr[2] << 8
            payload = f.read(length)
            if kind == ord("O"):
                for reply in endpoint.output(payload):
                    sock.sendall(
                        bytes([ord("I"), len(reply) & 0xFF, len(reply) >> 8]) + reply
                    )
            elif kind in (ord("G"), ord("S")):
                # no feature reports on these devices
                reply = struct.pack("<i", -32)  # -EPIPE, like a stalled request
                sock.sendall(bytes([kind + 32, len(reply), 0]) + reply)
        log(f"close {endpoint.path}")


def main():
    parser = argparse.ArgumentParser(description="HID++ device emulator for libratbag")
    parser.add_argument("--socket", default="/tmp/ratbag-emu.sock")
    parser.add_argument(
        "--mode",
        choices=["wired", "receiver", "both"],
        default="receiver",
        help="G703 by cable, behind a Lightspeed receiver, or both",
    )
    parser.add_argument(
        "--offline", action="store_true", help="paired device is switched off"
    )
    parser.add_argument("--state", help="file to keep the emulated flash in")
    args = parser.parse_args()

    mouse = G703(args.state)
    endpoints = []
    if args.mode in ("wired", "both"):
        g = "usb-1-046d-c087"
        endpoints += [
            Endpoint(
                "emu:wired:0",
                g,
                0x046D,
                0xC087,
                0,
                "G703 Wired/Wireless Gaming Mouse",
                MOUSE_DESCRIPTOR,
            ),
            Endpoint(
                "emu:wired:1",
                g,
                0x046D,
                0xC087,
                1,
                "G703 Wired/Wireless Gaming Mouse",
                HIDPP_DESCRIPTOR,
                WiredG703(mouse),
            ),
        ]
    if args.mode in ("receiver", "both"):
        g = "usb-2-046d-c539"
        endpoints += [
            Endpoint(
                "emu:receiver:0", g, 0x046D, 0xC539, 0, "USB Receiver", MOUSE_DESCRIPTOR
            ),
            Endpoint(
                "emu:receiver:2",
                g,
                0x046D,
                0xC539,
                2,
                "USB Receiver",
                HIDPP_DESCRIPTOR,
                LightspeedReceiver(mouse, not args.offline),
            ),
        ]

    if os.path.exists(args.socket):
        os.unlink(args.socket)
    server = Server(args.socket, endpoints)
    log(f"listening on {args.socket} ({args.mode})")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        os.unlink(args.socket)


if __name__ == "__main__":
    main()
