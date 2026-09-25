#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
import importlib.util
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
import zlib

SCRIPT = Path(__file__).resolve().parents[1] / 'be5000-calibration.py'
spec = importlib.util.spec_from_file_location('calibration', SCRIPT)
cal = importlib.util.module_from_spec(spec)
spec.loader.exec_module(cal)


def fixture():
    data = bytearray(b'\xff' * cal.EEPROM_SIZE)
    data[:2] = b'\x92\x79'
    data[4:10] = bytes.fromhex('020000000001')
    return bytes(data)


def environment(entries=None):
    if entries is None:
        entries = (b'ethaddr=02:00:00:00:00:10\0'
                   b'ethaddr_wan2=02:00:00:00:00:11\0'
                   b'ethaddr_wan=02:00:00:00:00:99\0'
                   b'password=private-test-value')
    body = entries + b'\0\0'
    body += b'\xff' * (cal.ENV_SIZE - 4 - len(body))
    return zlib.crc32(body).to_bytes(4, 'little') + body


class CalibrationTest(unittest.TestCase):
    def test_factory_wired_macs_only(self):
        for data in (environment(), environment() + b'\xff' * (0x80000 - cal.ENV_SIZE)):
            block = cal.prepare(fixture(), data)
            record = block[cal.MAC_OFFSET:cal.MAC_OFFSET + cal.MAC_RECORD_SIZE]
            self.assertEqual(record[:8], cal.MAC_MAGIC)
            self.assertEqual(record[8:20], bytes.fromhex('020000000010020000000011'))
            self.assertEqual(block[:cal.EEPROM_SIZE], fixture())
            self.assertNotIn(b'private-test-value', block)
            self.assertNotIn(bytes.fromhex('020000000099'), record)
            self.assertEqual(cal.prepare(block), block)
            self.assertEqual(cal.prepare(block, data), block)

    def test_reject_bad_factory_environment(self):
        cases = [b'', environment()[:-1], b'\0' * 4 + environment()[4:],
                 environment() + b'\0' * (0x80000 - cal.ENV_SIZE),
                 environment(b'ethaddr=02:00:00:00:00:10'),
                 environment(b'ethaddr=invalid\0ethaddr_wan2=02:00:00:00:00:11'),
                 environment(b'ethaddr=01:00:00:00:00:10\0ethaddr_wan2=02:00:00:00:00:11'),
                 environment(b'ethaddr=02:00:00:00:00:10\0ethaddr_wan2=02:00:00:00:00:10'),
                 environment(b'ethaddr=02:00:00:00:00:10\0ethaddr=02:00:00:00:00:10\0'
                             b'ethaddr_wan2=02:00:00:00:00:11')]
        for data in cases:
            with self.assertRaises(ValueError):
                cal.prepare(fixture(), data)

    def test_reject_corrupt_or_conflicting_record(self):
        block = cal.prepare(fixture(), environment())
        for offset in range(cal.MAC_RECORD_SIZE):
            corrupt = bytearray(block)
            corrupt[cal.MAC_OFFSET + offset] ^= 1
            with self.assertRaises(ValueError):
                cal.prepare(corrupt)
        other = environment(b'ethaddr=02:00:00:00:00:20\0ethaddr_wan2=02:00:00:00:00:21')
        with self.assertRaises(ValueError):
            cal.prepare(block, other)

    def test_supported_inputs_preserve_eeprom(self):
        eeprom = fixture()
        expected = eeprom + b'\xff' * (cal.BLOCK_SIZE - len(eeprom))
        for size in (cal.EEPROM_SIZE, cal.BLOCK_SIZE, cal.FACTORY_SIZE):
            self.assertEqual(cal.prepare(eeprom + b'\xff' * (size - len(eeprom))), expected)

    def test_reject_unknown_layout(self):
        for data in (b'', fixture()[:-1], b'\0\0' + fixture()[2:],
                     fixture() + b'\0' * (cal.BLOCK_SIZE - cal.EEPROM_SIZE)):
            with self.assertRaises(ValueError):
                cal.prepare(data)

    def test_reject_invalid_mac(self):
        for mac in (b'\0' * 6, b'\xff' * 6, bytes.fromhex('010000000001')):
            data = bytearray(fixture())
            data[4:10] = mac
            with self.assertRaises(ValueError):
                cal.prepare(data)

    def test_private_output_and_no_overwrite(self):
        with tempfile.TemporaryDirectory() as directory:
            source, output = (Path(directory) / name for name in ('factory', 'calibration'))
            source.write_bytes(fixture())
            command = [sys.executable, str(SCRIPT), str(source), str(output)]
            subprocess.run(command, check=True, capture_output=True)
            self.assertEqual(output.stat().st_mode & 0o777, 0o600)
            before = output.read_bytes()
            self.assertNotEqual(subprocess.run(command, capture_output=True).returncode, 0)
            self.assertEqual(output.read_bytes(), before)
            self.assertEqual(source.read_bytes(), fixture())


if __name__ == '__main__':
    unittest.main()
