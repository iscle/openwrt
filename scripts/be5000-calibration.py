#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Prepare BE5000 calibration storage from this router's own Factory backup.

This tool does not write flash and must never be used with another unit's data.
The generic sysupgrade image does not contain this output file.
"""
import argparse
import hashlib
import os
from pathlib import Path

EEPROM_SIZE = 0x1E00
BLOCK_SIZE = 0x20000
FACTORY_SIZE = 0x400000


def prepare(data):
    if len(data) not in (EEPROM_SIZE, BLOCK_SIZE, FACTORY_SIZE):
        raise ValueError("expected a 7680-byte EEPROM, 128-KiB block or 4-MiB Factory backup")
    eeprom = data[:EEPROM_SIZE]
    if eeprom[:2] != b"\x92\x79":
        raise ValueError("Factory backup does not identify an MT7992 radio")
    mac = eeprom[4:10]
    if mac[0] & 1 or mac in (b"\0" * 6, b"\xff" * 6):
        raise ValueError("Factory backup has an invalid base MAC address")
    if any(value != 0xFF for value in data[EEPROM_SIZE:]):
        raise ValueError("unrecognized Factory layout: refusing to discard trailing data")
    return eeprom + b"\xff" * (BLOCK_SIZE - EEPROM_SIZE)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("factory", type=Path, help="this router's original Factory backup")
    parser.add_argument("output", type=Path, help="private calibration block; never publish it")
    args = parser.parse_args()
    try:
        block = prepare(args.factory.read_bytes())
        # Exclusive creation prevents replacing a backup by mistake.
        fd = os.open(args.output, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        with os.fdopen(fd, "wb") as output:
            output.write(block)
    except (OSError, ValueError) as error:
        parser.exit(1, f"error: {error}\n")
    print(f"Prepared {len(block)} bytes; SHA-256 {hashlib.sha256(block).hexdigest()}")
    print("Keep this per-device file private. Verify the destination block before provisioning.")


if __name__ == "__main__":
    main()
