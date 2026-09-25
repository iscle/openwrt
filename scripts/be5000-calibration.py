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
import re
import zlib

EEPROM_SIZE = 0x1E00
BLOCK_SIZE = 0x20000
FACTORY_SIZE = 0x400000
MAC_OFFSET = 0x2000
MAC_MAGIC = b"BE5MAC01"
MAC_RECORD_SIZE = 24
ENV_SIZE = 0x10000


def valid_mac(mac):
    return len(mac) == 6 and not mac[0] & 1 and mac != b"\0" * 6


def factory_macs(data):
    """Read only factory wired identities from a CRC-checked stock bdata."""
    if len(data) not in (ENV_SIZE, 0x80000):
        raise ValueError("expected a 64-KiB environment or 512-KiB bdata backup")
    if int.from_bytes(data[:4], "little") != zlib.crc32(data[4:ENV_SIZE]):
        raise ValueError("bdata CRC mismatch")
    if any(value != 0xFF for value in data[ENV_SIZE:]):
        raise ValueError("unrecognized data after the bdata environment")
    end = data.find(b"\0\0", 4, ENV_SIZE)
    if end < 0:
        raise ValueError("unterminated bdata environment")
    addresses = {}
    for entry in data[4:end].split(b"\0"):
        key, separator, value = entry.partition(b"=")
        if key not in (b"ethaddr", b"ethaddr_wan2"):
            continue
        if not separator or key in addresses or not re.fullmatch(
                rb"[0-9a-fA-F]{2}(?::[0-9a-fA-F]{2}){5}", value):
            raise ValueError("invalid or duplicate factory MAC field")
        mac = bytes.fromhex(value.decode().replace(":", ""))
        if not valid_mac(mac):
            raise ValueError("invalid factory wired MAC address")
        addresses[key] = mac
    if len(addresses) != 2 or addresses[b"ethaddr"] == addresses[b"ethaddr_wan2"]:
        raise ValueError("missing or duplicate factory wired identities")
    # ethaddr_wan can contain the user's WAN MAC override. Do not migrate it.
    return addresses[b"ethaddr"] + addresses[b"ethaddr_wan2"]


def prepare(data, bdata=None):
    if len(data) not in (EEPROM_SIZE, BLOCK_SIZE, FACTORY_SIZE):
        raise ValueError("expected a 7680-byte EEPROM, 128-KiB block or 4-MiB Factory backup")
    eeprom = data[:EEPROM_SIZE]
    if eeprom[:2] != b"\x92\x79":
        raise ValueError("Factory backup does not identify an MT7992 radio")
    mac = eeprom[4:10]
    if not valid_mac(mac):
        raise ValueError("Factory backup has an invalid base MAC address")
    record = data[MAC_OFFSET:MAC_OFFSET + MAC_RECORD_SIZE]
    tail = data[EEPROM_SIZE:]
    if record.startswith(MAC_MAGIC) and len(data) == BLOCK_SIZE:
        if (len(record) != MAC_RECORD_SIZE or
                int.from_bytes(record[20:24], "little") != zlib.crc32(record[:20]) or
                not valid_mac(record[8:14]) or not valid_mac(record[14:20]) or
                record[8:14] == record[14:20]):
            raise ValueError("invalid wired MAC record")
        if any(value != 0xFF for value in data[EEPROM_SIZE:MAC_OFFSET]):
            raise ValueError("unrecognized data before the wired MAC record")
        tail = data[MAC_OFFSET + MAC_RECORD_SIZE:]
    else:
        record = b""
    if any(value != 0xFF for value in tail):
        raise ValueError("unrecognized Factory layout: refusing to discard trailing data")
    if bdata is not None:
        payload = MAC_MAGIC + factory_macs(bdata)
        replacement = payload + zlib.crc32(payload).to_bytes(4, "little")
        if record and replacement != record:
            raise ValueError("bdata identities disagree with the prepared block")
        record = replacement
    result = eeprom + b"\xff" * (MAC_OFFSET - EEPROM_SIZE) + record
    return result + b"\xff" * (BLOCK_SIZE - len(result))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("factory", type=Path, help="this router's original Factory backup")
    parser.add_argument("output", type=Path, help="private calibration block; never publish it")
    parser.add_argument("--bdata", type=Path,
                        help="this router's original bdata backup, for wired MAC addresses")
    args = parser.parse_args()
    try:
        block = prepare(args.factory.read_bytes(),
                        args.bdata.read_bytes() if args.bdata else None)
        # Exclusive creation prevents replacing a backup by mistake.
        fd = os.open(args.output, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        with os.fdopen(fd, "wb") as output:
            output.write(block)
    except (OSError, ValueError) as error:
        parser.exit(1, f"error: {error}\n")
    print(f"Prepared {len(block)} bytes; SHA-256 {hashlib.sha256(block).hexdigest()}")
    if block[MAC_OFFSET:MAC_OFFSET + len(MAC_MAGIC)] != MAC_MAGIC:
        print("Wired MAC addresses are absent; supply --bdata to preserve them.")
    print("Keep this per-device file private. Verify the destination block before provisioning.")


if __name__ == "__main__":
    main()
