#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
import importlib.util
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

SCRIPT = Path(__file__).resolve().parents[1] / 'be5000-calibration.py'
spec = importlib.util.spec_from_file_location('calibration', SCRIPT)
cal = importlib.util.module_from_spec(spec)
spec.loader.exec_module(cal)


def fixture():
    data = bytearray(b'\xff' * cal.EEPROM_SIZE)
    data[:2] = b'\x92\x79'
    data[4:10] = bytes.fromhex('020000000001')
    return bytes(data)


class CalibrationTest(unittest.TestCase):
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
