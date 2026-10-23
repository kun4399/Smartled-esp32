#!/usr/bin/env python3
"""Generate an ignored ESP-IDF header from the private gatt_ltmk file."""

from __future__ import annotations

import argparse
from pathlib import Path
import stat

DEFAULT_KEY = Path.home() / ".config/smartled-esp32/gatt_ltmk.hex"
OUTPUT = Path(__file__).resolve().parents[1] / "main/gatt_ltmk_private.h"


def read_key(path: Path) -> bytes:
    mode = stat.S_IMODE(path.stat().st_mode)
    if mode & 0o077:
        raise ValueError("key file must be private (0600 or stricter)")
    try:
        key = bytes.fromhex(path.read_text(encoding="ascii").strip())
    except (OSError, UnicodeError, ValueError):
        raise ValueError("key file is unreadable or malformed") from None
    if len(key) != 32:
        raise ValueError("key file must contain exactly 32 bytes")
    return key


def generate_header(key: bytes) -> bytes:
    rows = [", ".join(f"0x{value:02x}" for value in key[index:index + 8])
            for index in range(0, len(key), 8)]
    source = "#pragma once\n#include <stdint.h>\n\nstatic const uint8_t SMARTLED_GATT_LTMK[32] = {\n"
    source += "\n".join(f"    {row}," for row in rows)
    source += "\n};\n"
    return source.encode("ascii")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--key", type=Path, default=DEFAULT_KEY)
    args = parser.parse_args()
    key = read_key(args.key.expanduser())
    temp = OUTPUT.with_suffix(".h.tmp")
    temp.write_bytes(generate_header(key))
    temp.chmod(0o600)
    temp.replace(OUTPUT)
    OUTPUT.chmod(0o600)
    print("Private firmware key header generated at the ignored project path; key value was not displayed.")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError) as exc:
        raise SystemExit(f"Key provisioning failed: {exc}")
