#!/usr/bin/env python3
"""Read-only macOS BLE survey for lemesh.light.wy0d02 (PID 19733).

Uses CoreBluetooth through Bleak. No GATT reads, writes, pairing or provisioning.
The advertised FE95 service-data value starts with MiBeacon frame control, PID,
then frame counter (the FE95 UUID is not included in Bleak's service_data bytes).
"""

import argparse
import asyncio
import re
import sys

from bleak import BleakClient, BleakScanner

TARGET_PID = 19733
FE95_SUFFIX = "0000fe95-0000-1000-8000-00805f9b34fb"


def product_id(service_data):
    for uuid, payload in service_data.items():
        if uuid.lower() == FE95_SUFFIX or uuid.lower() == "fe95":
            if len(payload) >= 5:
                return int.from_bytes(payload[2:4], "little")
    return None


async def inspect(device):
    print("\nGATT discovery: selected unique target candidate", flush=True)
    try:
        async with BleakClient(device, timeout=15.0) as client:
            for service in client.services:
                print(f"  service {service.uuid}")
                for char in service.characteristics:
                    print(
                        f"    char {char.uuid} handle={char.handle} "
                        f"properties={','.join(char.properties)}"
                    )
    except Exception as exc:
        message = re.sub(r"(?i)(?:[0-9a-f]{2}[:-]){5}[0-9a-f]{2}", "<BLE_ADDR>", str(exc))
        message = re.sub(r"(?i)\b[0-9a-f]{8}(?:-[0-9a-f]{4}){3}-[0-9a-f]{12}\b", "<BLE_ID>", message)
        print(f"GATT discovery failed: {type(exc).__name__}: {message}", file=sys.stderr)
        return 2
    return 0


async def run(args):
    print(f"Scanning for {args.seconds:g} seconds...", flush=True)
    try:
        found = await BleakScanner.discover(timeout=args.seconds, return_adv=True)
    except Exception as exc:
        print(f"BLE scan failed: {type(exc).__name__}: {exc}", file=sys.stderr)
        print("Check that Bluetooth is on and Terminal has macOS Bluetooth permission.", file=sys.stderr)
        return 2

    candidates = []
    for device, adv in found.values():
        pid = product_id(adv.service_data)
        if pid != TARGET_PID and not args.all:
            continue
        label = "lemesh.light.wy0d02 candidate" if pid == TARGET_PID else "other/unknown"
        print(
            f"{label}: name={adv.local_name or device.name or '(none)'} "
            f"rssi={adv.rssi} pid={pid if pid is not None else 'unknown'}"
        )
        if args.all or pid == TARGET_PID:
            print(f"  service_uuids={','.join(adv.service_uuids) or '(none)'}")
            print(f"  service_data_uuids={','.join(adv.service_data) or '(none)'}")
        if pid == TARGET_PID:
            candidates.append(device)

    print(f"Scan complete: {len(found)} BLE identities, {len(candidates)} PID-19733 candidates.")
    if args.inspect_target:
        if len(candidates) != 1:
            print("GATT inspection requires exactly one PID-19733 candidate; none was selected.")
            return 1
        return await inspect(candidates[0])
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--seconds", type=float, default=15.0, help="scan time (default: 15)")
    parser.add_argument("--all", action="store_true", help="show all discovered devices")
    parser.add_argument(
        "--inspect-target", action="store_true",
        help="connect to the sole PID-19733 candidate and list GATT UUIDs (read-only)",
    )
    args = parser.parse_args()
    if not 1 <= args.seconds <= 120:
        parser.error("--seconds must be 1..120")
    raise SystemExit(asyncio.run(run(args)))


if __name__ == "__main__":
    main()
