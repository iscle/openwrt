#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Exercise the real image size check at U-Boot's flash read boundary."""
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class ImageSizeTest(unittest.TestCase):
    def test_flash_fit_must_fit_bootloader_read(self):
        env = (ROOT / 'package/boot/uboot-airoha/patches/'
               '310-airoha-add-support-for-Airoha-AN7563-SoC.patch').read_text()
        limit = int(re.search(r'^\+fitsize=(0x[0-9a-fA-F]+)$', env, re.M)[1], 16)
        image_mk = (ROOT / 'include/image.mk').read_text()
        units = re.search(r'^exp_units = .*$', image_mk, re.M)[0]
        # Device/Build/kernel invokes this same checker with KERNEL_SIZE.
        # Use the real device template, not a duplicated size constant.
        makefile = f'''{units}
include {ROOT}/include/image-commands.mk
include {ROOT}/target/linux/airoha/image/an7563.mk
$(eval $(Device/xiaomi_be5000))
ERROR_MESSAGE = echo '$(1)' >&2
.PHONY: FORCE
fit: FORCE
\t$(call Build/check-size,$(KERNEL_SIZE))
'''
        with tempfile.TemporaryDirectory(prefix='be5000-image-') as tmp:
            directory = Path(tmp)
            (directory / 'Makefile').write_text(makefile)
            for size in (limit - 1, limit, limit + 1):
                with self.subTest(size=size):
                    image = directory / 'fit'
                    with image.open('wb') as output:
                        output.truncate(size)
                    result = subprocess.run(['make', '--no-print-directory', 'fit'],
                                            cwd=directory, text=True,
                                            capture_output=True, check=True)
                    self.assertEqual(image.exists(), size <= limit,
                                     result.stdout + result.stderr)
                    if size > limit:
                        self.assertIn('too big', result.stderr)


if __name__ == '__main__':
    unittest.main()
