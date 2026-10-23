#!/usr/bin/env python3
"""Authenticate to a Xiaomi BLE candidate and inspect/control light properties.

Default behavior reads the power, brightness, and color-temperature properties.
Explicit control options write only the requested properties and verify them by
read-back. No bind/reset/provisioning commands are implemented.
"""

from __future__ import annotations

import argparse
import asyncio
import hashlib
import hmac
from pathlib import Path
import re
import stat
import sys

from bleak import BleakClient, BleakScanner
from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.hazmat.primitives.ciphers.aead import AESCCM

TARGET_NAME = "lemesh.light.wy0d02"
TARGET_PID = 19733
FE95_UUIDS = {"fe95", "0000fe95-0000-1000-8000-00805f9b34fb"}
DEFAULT_KEY = Path.home() / ".config/smartled-esp32/gatt_ltmk.hex"
UUID_CTRL = "00000010-0000-1000-8000-00805f9b34fb"
UUID_AUTH = "00000016-0000-1000-8000-00805f9b34fb"
UUID_CMD = "0000001a-0000-1000-8000-00805f9b34fb"
UUID_DAT = "0000001b-0000-1000-8000-00805f9b34fb"
UUID_MCU = "0000001c-0000-1000-8000-00805f9b34fb"
HKDF_SALT = b"miot-mesh-login-salt"
HKDF_INFO = b"miot-mesh-login-info"


def hkdf_sha256(ikm: bytes, salt: bytes, info: bytes, length: int) -> bytes:
    prk = hmac.new(salt, ikm, hashlib.sha256).digest()
    out, block, counter = bytearray(), b"", 1
    while len(out) < length:
        block = hmac.new(prk, block + info + bytes([counter]), hashlib.sha256).digest()
        out.extend(block)
        counter += 1
    return bytes(out[:length])


def pack_spec(tid: int, opcode: int, body: bytes = b"") -> bytes:
    if not 0 <= tid <= 0xffff or not 0 <= opcode <= 0xff:
        raise ValueError("invalid MIoT packet field")
    size = 5 + len(body)
    return (0x2000 | size).to_bytes(2, "little") + tid.to_bytes(2, "little") + bytes([opcode]) + body


def get_property_packet(tid: int, siid: int, piid: int) -> bytes:
    return pack_spec(tid, 2, bytes([1, siid]) + piid.to_bytes(2, "little"))


def set_bool_property_packet(tid: int, siid: int, piid: int, value: bool) -> bytes:
    # MIoT Spec-v2 uses type 0 for BOOL and little-endian (type << 12) | length.
    body = bytes([1, siid]) + piid.to_bytes(2, "little") + b"\x01\x00" + bytes([int(value)])
    return pack_spec(tid, 0, body)


def set_uint_property_packet(tid: int, siid: int, piid: int, value: int, width: int) -> bytes:
    if width not in (1, 2, 4) or not 0 <= value < (1 << (width * 8)):
        raise ValueError("unsigned MIoT property value does not fit its type")
    value_type = {1: 1, 2: 3, 4: 5}[width]
    type_len = ((value_type << 12) | width).to_bytes(2, "little")
    body = bytes([1, siid]) + piid.to_bytes(2, "little") + type_len + value.to_bytes(width, "little")
    return pack_spec(tid, 0, body)


def parse_set_property(data: bytes) -> dict:
    if len(data) < 6:
        raise ValueError("short MIoT SetProperty response")
    header = int.from_bytes(data[:2], "little")
    if header >> 12 != 2 or (header & 0x0fff) != len(data):
        raise ValueError("invalid MIoT SetProperty response header")
    tid = int.from_bytes(data[2:4], "little")
    if data[4] != 1:
        raise ValueError("response was not SetPropertyResp")
    count = data[5]
    offset = 6
    results = []
    for _ in range(count):
        if offset + 5 > len(data):
            raise ValueError("truncated SetProperty result")
        results.append({
            "siid": data[offset],
            "piid": int.from_bytes(data[offset + 1:offset + 3], "little"),
            "code": int.from_bytes(data[offset + 3:offset + 5], "little"),
        })
        offset += 5
    if offset != len(data):
        raise ValueError("unexpected trailing SetProperty data")
    return {"tid": tid, "opcode": 1, "properties": results}


def parse_get_property(data: bytes) -> dict:
    if len(data) < 6:
        raise ValueError("short MIoT response")
    header = int.from_bytes(data[:2], "little")
    if header >> 12 != 2 or (header & 0x0fff) != len(data):
        raise ValueError("invalid MIoT response header")
    tid = int.from_bytes(data[2:4], "little")
    opcode = data[4]
    if opcode != 3:
        raise ValueError("response was not GetPropertyResp")
    count = data[5]
    offset = 6
    result = []
    for _ in range(count):
        if offset + 5 > len(data):
            raise ValueError("truncated property result")
        siid = data[offset]
        piid = int.from_bytes(data[offset + 1:offset + 3], "little")
        status = int.from_bytes(data[offset + 3:offset + 5], "little")
        offset += 5
        row = {"siid": siid, "piid": piid, "status": status}
        if status == 0:
            if offset + 2 > len(data):
                raise ValueError("truncated property type")
            type_len = int.from_bytes(data[offset:offset + 2], "little")
            value_type, value_len = type_len >> 12, type_len & 0x0fff
            offset += 2
            if offset + value_len > len(data):
                raise ValueError("truncated property value")
            raw = data[offset:offset + value_len]
            offset += value_len
            if value_type == 0 and value_len == 1:
                row["value"] = bool(raw[0])
                row["type"] = "bool"
            elif value_type in (1, 2, 3, 4, 5, 6, 7, 8) and value_len in (1, 2, 4, 8):
                row["value"] = int.from_bytes(raw, "little", signed=value_type in (2, 4, 6, 8))
                row["type"] = value_type
            else:
                row["value"] = "<non-scalar>"
                row["type"] = value_type
        result.append(row)
    if offset != len(data):
        raise ValueError("unexpected trailing property data")
    return {"tid": tid, "opcode": opcode, "properties": result}


def read_key(path: Path) -> bytes:
    path = path.expanduser()
    mode = stat.S_IMODE(path.stat().st_mode)
    if mode & 0o077:
        raise ValueError("gatt_ltmk file permissions must be 0600 or stricter")
    try:
        key = bytes.fromhex(path.read_text(encoding="ascii").strip())
    except (OSError, UnicodeError, ValueError):
        raise ValueError("gatt_ltmk file is unreadable or malformed") from None
    if len(key) != 32:
        raise ValueError("gatt_ltmk file must contain exactly 32 bytes")
    return key


def is_target_advertisement(device, advertisement) -> bool:
    if advertisement.local_name == TARGET_NAME or device.name == TARGET_NAME:
        return True
    for uuid, payload in advertisement.service_data.items():
        if uuid.lower() in FE95_UUIDS and len(payload) >= 5:
            if int.from_bytes(payload[2:4], "little") == TARGET_PID:
                return True
    return False


class MiotBleProbe:
    def __init__(self, client: BleakClient, ltmk: bytes):
        self.client = client
        self.ltmk = ltmk
        self.key: bytes | None = None
        self.dmtu = 242
        self.auth_rx: asyncio.Queue[bytes] = asyncio.Queue()
        self.auth_packet_rx: asyncio.Queue[bytes] = asyncio.Queue()
        self.ctrl_rx: asyncio.Queue[bytes] = asyncio.Queue()
        self.cmd_rx: asyncio.Queue[bytes] = asyncio.Queue()
        self.dat_rx: asyncio.Queue[bytes] = asyncio.Queue()
        self.pending: dict[int, asyncio.Future] = {}
        self.tx_counter = 1
        self.roll = 0
        self._tasks: list[asyncio.Task] = []

    def _nonce(self, iv: bytes, counter: int, roll: int = 0) -> bytes:
        return iv + bytes(4) + counter.to_bytes(2, "little") + roll.to_bytes(2, "little")

    def _encrypt(self, packet: bytes, counter: int) -> bytes:
        assert self.key is not None
        encrypted = AESCCM(self.key[16:32], tag_length=4).encrypt(
            self._nonce(self.key[36:40], counter, self.roll), packet, None
        )
        return counter.to_bytes(2, "little") + encrypted

    def _decrypt(self, frame: bytes) -> tuple[int, bytes]:
        assert self.key is not None
        if len(frame) < 6:
            raise ValueError("encrypted response too short")
        counter = int.from_bytes(frame[:2], "little")
        packet = AESCCM(self.key[:16], tag_length=4).decrypt(
            self._nonce(self.key[32:36], counter), frame[2:], None
        )
        return counter, packet

    @staticmethod
    def _notify(queue: asyncio.Queue[bytes]):
        def callback(_sender, data):
            queue.put_nowait(bytes(data))
        return callback

    async def _auth_handler(self):
        while True:
            frame = await self.auth_rx.get()
            if len(frame) < 4 or frame[0:2] != b"\0\0":
                continue
            kind, ident, payload = frame[2], frame[3], frame[4:]
            if kind == 4 and ident == 0 and len(payload) >= 2:
                self.dmtu = min(242, payload[1])
                await self.client.write_gatt_char(UUID_AUTH, b"\0\0\x05\0\x06" + bytes([self.dmtu]), response=False)
            elif kind == 4 and ident == 1:
                size = len(payload) + 2 if len(payload) + 2 == self.dmtu else 18
                self.dmtu = size
                await self.client.write_gatt_char(UUID_AUTH, b"\0\0\x05\x01" + bytes([size]) * (size - 2), response=False)
            elif kind == 2 and ident == 3:
                await self.auth_packet_rx.put(payload)
            elif kind == 0:
                await self.client.write_gatt_char(UUID_AUTH, bytes([0, 0, 1, ident, 1, 0]), response=False)

    async def _data_handler(self):
        while True:
            frame = await self.dat_rx.get()
            if frame.startswith(b"\0\0\x02\0"):
                await self.client.write_gatt_char(UUID_DAT, b"\0\0\x03\0", response=False)
                try:
                    _counter, packet = self._decrypt(frame[4:])
                    if packet[4] == 3:
                        response = parse_get_property(packet)
                    elif packet[4] == 1:
                        response = parse_set_property(packet)
                    else:
                        response = {"tid": int.from_bytes(packet[2:4], "little"), "opcode": packet[4]}
                except (ValueError, IndexError):
                    continue
                future = self.pending.pop(response["tid"], None)
                if future and not future.done():
                    future.set_result(response)

    async def start(self):
        await self.client.start_notify(UUID_AUTH, self._notify(self.auth_rx))
        await self.client.start_notify(UUID_CTRL, self._notify(self.ctrl_rx))
        await self.client.start_notify(UUID_CMD, self._notify(self.cmd_rx))
        await self.client.start_notify(UUID_DAT, self._notify(self.dat_rx))
        await self.client.start_notify(UUID_MCU, lambda _sender, _data: None)
        self._tasks = [asyncio.create_task(self._auth_handler()), asyncio.create_task(self._data_handler())]

    async def _send_auth_package(self, package_id: int, payload: bytes):
        count = max(1, (len(payload) + self.dmtu - 1) // self.dmtu)
        await self.client.write_gatt_char(UUID_AUTH, b"\0\0\0" + bytes([package_id]) + count.to_bytes(2, "little"), response=False)
        await asyncio.sleep(0.35)
        for sequence in range(count):
            chunk = payload[sequence * self.dmtu:(sequence + 1) * self.dmtu]
            await self.client.write_gatt_char(UUID_AUTH, (sequence + 1).to_bytes(2, "little") + chunk, response=False)
            await asyncio.sleep(0.12)

    async def login(self):
        # MCU capability exchange and protocol negotiation; no binding or reset.
        for packet in (b"\0", b"\x08\x01\0", b"\x03"):
            await self.client.write_gatt_char(UUID_MCU, packet, response=False)
            await asyncio.sleep(0.25)
        await self.client.write_gatt_char(UUID_CTRL, b"\xa4", response=False)
        await asyncio.sleep(2.2)
        private = ec.generate_private_key(ec.SECP256R1())
        public = private.public_key().public_bytes(serialization.Encoding.X962, serialization.PublicFormat.UncompressedPoint)[1:]
        await self.client.write_gatt_char(UUID_CTRL, bytes.fromhex("50000000"), response=False)
        await asyncio.sleep(0.2)
        await self._send_auth_package(3, public)
        device_public = None
        deadline = asyncio.get_running_loop().time() + 4
        while asyncio.get_running_loop().time() < deadline:
            try:
                frame = await asyncio.wait_for(self.auth_packet_rx.get(), 0.3)
            except asyncio.TimeoutError:
                continue
            if len(frame) >= 64:
                device_public = frame[:64]
                break
        if device_public is None:
            raise RuntimeError("device ECDH key was not received")
        await self.client.write_gatt_char(UUID_AUTH, b"\0\0\x03\0", response=False)
        shared = private.exchange(ec.ECDH(), ec.EllipticCurvePublicKey.from_encoded_point(ec.SECP256R1(), b"\x04" + device_public))
        self.key = hkdf_sha256(shared + self.ltmk, HKDF_SALT, HKDF_INFO, 64)
        import zlib
        proof = AESCCM(self.key[16:32], tag_length=4).encrypt(
            bytes(range(0x10, 0x1c)), zlib.crc32(device_public).to_bytes(4, "little"), None
        )
        await self._send_auth_package(5, proof)
        deadline = asyncio.get_running_loop().time() + 4
        authenticated = False
        while asyncio.get_running_loop().time() < deadline:
            try:
                frame = await asyncio.wait_for(self.ctrl_rx.get(), 0.2)
            except asyncio.TimeoutError:
                continue
            if frame[:1] == b"\x51":
                authenticated = True
                break
            if frame[:1] == b"\x52":
                raise RuntimeError("device rejected MIoT BLE login")
        if not authenticated:
            raise RuntimeError("MIoT BLE login response timed out")

        # F0 version exchange must immediately follow a successful login.
        future = asyncio.get_running_loop().create_future()
        self.pending[1] = future
        await self._send_encrypted(pack_spec(1, 0xF0), 0)
        try:
            await asyncio.wait_for(future, 2)
        except asyncio.TimeoutError:
            self.pending.pop(1, None)
            print("Protocol version exchange had no response; continuing to read-only test.")

    async def _send_encrypted(self, packet: bytes, counter: int):
        await self.client.write_gatt_char(UUID_CMD, b"\0\0\0\0\x01\0", response=False)
        deadline = asyncio.get_running_loop().time() + 1.2
        while asyncio.get_running_loop().time() < deadline:
            try:
                ack = await asyncio.wait_for(self.cmd_rx.get(), 0.15)
            except asyncio.TimeoutError:
                continue
            if ack.startswith(b"\0\0\x01\x01"):
                break
        else:
            raise RuntimeError("data channel did not become ready")
        await self.client.write_gatt_char(UUID_CMD, b"\x01\0" + self._encrypt(packet, counter), response=False)

    def _next_tx_counter(self) -> int:
        counter = self.tx_counter
        self.tx_counter = (self.tx_counter + 1) & 0xffff
        return counter

    async def get_property(self, siid: int, piid: int) -> dict:
        tid = (0x7200 + self.tx_counter) & 0xffff
        future = asyncio.get_running_loop().create_future()
        self.pending[tid] = future
        try:
            await self._send_encrypted(get_property_packet(tid, siid, piid), self._next_tx_counter())
            return await asyncio.wait_for(future, 4)
        except asyncio.TimeoutError:
            self.pending.pop(tid, None)
            raise RuntimeError(f"MIoT GetProperty timed out for {siid}.{piid}") from None
        except Exception:
            self.pending.pop(tid, None)
            raise

    async def set_property(self, siid: int, piid: int, value: bool | int, width: int | None = None) -> dict:
        tid = (0x7300 + self.tx_counter) & 0xffff
        future = asyncio.get_running_loop().create_future()
        self.pending[tid] = future
        packet = (set_bool_property_packet(tid, siid, piid, value) if width is None else
                  set_uint_property_packet(tid, siid, piid, int(value), width))
        try:
            await self._send_encrypted(packet, self._next_tx_counter())
            return await asyncio.wait_for(future, 4)
        except asyncio.TimeoutError:
            self.pending.pop(tid, None)
            raise RuntimeError("SetProperty response timed out") from None
        except Exception:
            self.pending.pop(tid, None)
            raise

    async def close(self):
        for task in self._tasks:
            task.cancel()
        if self._tasks:
            await asyncio.gather(*self._tasks, return_exceptions=True)
        if self.client.is_connected:
            await self.client.disconnect()


async def choose_device(address: str | None, seconds: float):
    if address:
        return address
    found = await BleakScanner.discover(timeout=seconds, return_adv=True)
    candidates = [(device, adv) for device, adv in found.values()
                  if is_target_advertisement(device, adv)]
    if not candidates:
        raise RuntimeError("no exact-model advertisement found by macOS BLE scan; ESP32 discovery remains available")
    if len(candidates) == 1:
        return candidates[0][0].address
    print(f"Found {len(candidates)} same-model BLE candidates (RSSI only; addresses hidden):")
    for index, (_device, adv) in enumerate(candidates, 1):
        print(f"  {index}. RSSI {adv.rssi} dBm")
    selection = int(input("Choose candidate number: "))
    if not 1 <= selection <= len(candidates):
        raise RuntimeError("candidate selection out of range")
    return candidates[selection - 1][0].address


async def run(args) -> int:
    key = read_key(args.key)
    address = await choose_device(args.address, args.scan_seconds)
    client = BleakClient(address, timeout=20)
    probe = MiotBleProbe(client, key)
    try:
        await client.connect()
        chars = {c.uuid.lower() for s in client.services for c in s.characteristics}
        required = {UUID_CTRL, UUID_AUTH, UUID_CMD, UUID_DAT, UUID_MCU}
        if not required.issubset(chars):
            raise RuntimeError("candidate is missing expected FE95 MIoT BLE characteristics")
        await probe.start()
        print("GATT connected; attempting MIoT BLE login (no bind/reset operation).")
        await probe.login()
        print("Login result: success (0x51).")
        properties = {
            "power": 1,
            "brightness": 2,
            "color_temperature": 3,
        }
        current = {}
        for name, piid in properties.items():
            response = await probe.get_property(2, piid)
            row = next((item for item in response["properties"]
                        if item["siid"] == 2 and item["piid"] == piid), None)
            if row is not None and row.get("status") == 0:
                current[name] = row.get("value")
            print(f"Current {name}: {row}")
        if args.turn_on:
            power = current.get("power")
            if not isinstance(power, bool):
                raise RuntimeError("power property was not readable; refusing to write")
            if power:
                print("Device already reports on; no write sent.")
            else:
                args._writes.append(("power", 1, True, None))
        if args.brightness is not None:
            args._writes.append(("brightness", 2, args.brightness, 1))
        if args.color_temperature is not None:
            args._writes.append(("color_temperature", 3, args.color_temperature, 2))

        for name, piid, desired, width in args._writes:
            if name != "power":
                if type(current.get(name)) is not int:
                    raise RuntimeError(f"{name} property was not readable as an integer; refusing to write")
            if name != "power" and current[name] == desired:
                print(f"{name} already equals {desired}; no write sent.")
                continue
            result = await probe.set_property(2, piid, desired, width)
            print(f"SetProperty response for {name}: {result}")
            write_row = next((item for item in result["properties"]
                              if item["siid"] == 2 and item["piid"] == piid), None)
            if write_row is None or write_row["code"] != 0:
                raise RuntimeError(f"device rejected the {name} property write")
            verified = await probe.get_property(2, piid)
            row = next((item for item in verified["properties"]
                        if item["siid"] == 2 and item["piid"] == piid), None)
            print(f"Read-back {name}: {row}")
            if row is None or row.get("status") != 0 or row.get("value") != desired:
                raise RuntimeError(f"{name} could not be confirmed by property read-back")
        return 0
    finally:
        await probe.close()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--key", type=Path, default=DEFAULT_KEY, help="private gatt_ltmk hex file")
    parser.add_argument("--address", help="optional BLE address; omit to scan by exact model name")
    parser.add_argument("--scan-seconds", type=float, default=25)
    parser.add_argument("--turn-on", action="store_true",
                        help="if currently off, set MIoT service 2/property 1 true and read it back")
    parser.add_argument("--brightness", type=int, metavar="1..100",
                        help="set brightness (MIoT service 2/property 2) and verify by read-back")
    parser.add_argument("--color-temperature", type=int, metavar="2700..6500",
                        help="set color temperature in kelvin (service 2/property 3) and verify")
    args = parser.parse_args()
    if not 5 <= args.scan_seconds <= 120:
        parser.error("--scan-seconds must be 5..120")
    if args.brightness is not None and not 1 <= args.brightness <= 100:
        parser.error("--brightness must be 1..100")
    if args.color_temperature is not None and not 2700 <= args.color_temperature <= 6500:
        parser.error("--color-temperature must be 2700..6500 K")
    args._writes = []
    try:
        return asyncio.run(run(args))
    except KeyboardInterrupt:
        print("Interrupted.", file=sys.stderr)
        return 130
    except Exception as exc:
        message = re.sub(r"(?i)(?:[0-9a-f]{2}[:-]){5}[0-9a-f]{2}", "<BLE_ADDR>", str(exc))
        message = re.sub(r"(?i)\b[0-9a-f]{8}(?:-[0-9a-f]{4}){3}-[0-9a-f]{12}\b", "<BLE_ID>", message)
        print(f"Probe failed: {type(exc).__name__}: {message}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
