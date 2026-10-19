#!/usr/bin/env python3
"""Exercise BE5000 provisioning with flash operations replaced by shell stubs."""
from pathlib import Path
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[2]
BOARD = ROOT / 'target/linux/airoha/an7563/base-files'


class UpgradeTests(unittest.TestCase):
    def run_shell(self, source, stubs, command):
        return subprocess.run(['sh'], input=stubs + '\n' + source + '\n' + command,
                              text=True, capture_output=True, check=False)

    def test_boot_never_formats_on_attachment_failure(self):
        source = (BOARD / 'lib/preinit/75_ubi_rootfs_data').read_text()
        source = source.replace('. /lib/upgrade/nand.sh', '')
        stubs = '''
board_name() { echo xiaomi,be5000; }
find_mtd_index() { echo 7; }
nand_find_ubi() { return 1; }
ubiattach() { echo ATTACH; return 1; }
ubiformat() { echo FORMAT; return 0; }
boot_hook_add() { :; }
'''
        result = self.run_shell(source, stubs, 'ubi_rootfs_data')
        self.assertNotIn('FORMAT', result.stdout)
        self.assertNotEqual(result.returncode, 0)
        result = self.run_shell(source, stubs, 'INITRAMFS=1; ubi_rootfs_data')
        self.assertEqual(result.returncode, 0)
        self.assertEqual(result.stdout, '')

    def upgrade(self, overrides=''):
        source = (BOARD / 'lib/upgrade/platform.sh').read_text()
        stubs = '''
find_mtd_index() { echo 7; }
nand_attach_ubi() { echo ubi0; }
get_image() { echo image; }
mtd() { cat >/dev/null; echo WRITE; }
nand_find_volume() { echo ubi0_0; }
ubirmvol() { echo REMOVE; }
ubimkvol() { echo CREATE; }
nand_restore_config() { echo RESTORE; }
UPGRADE_BACKUP=/tmp/backup.tgz
'''
        return self.run_shell(source, stubs + overrides,
                              'xiaomi_be5000_do_upgrade /tmp/image')

    def test_restore_failure_is_reported(self):
        result = self.upgrade('nand_restore_config() { echo RESTORE_FAILED; return 1; }')
        self.assertIn('RESTORE_FAILED', result.stdout)
        self.assertNotEqual(result.returncode, 0)

    def test_missing_partition_or_attachment_fails_before_write(self):
        for override in ['find_mtd_index() { return 1; }',
                         'nand_attach_ubi() { return 1; }']:
            result = self.upgrade(override)
            self.assertNotEqual(result.returncode, 0)
            self.assertNotIn('WRITE', result.stdout)

    def test_removal_failure_does_not_create_volume(self):
        result = self.upgrade('ubirmvol() { return 1; }')
        self.assertNotEqual(result.returncode, 0)
        self.assertNotIn('CREATE', result.stdout)

    def test_success_restores_backup(self):
        result = self.upgrade()
        self.assertEqual(result.returncode, 0)
        self.assertEqual(result.stdout.splitlines(), ['WRITE', 'REMOVE', 'CREATE', 'RESTORE'])


if __name__ == '__main__':
    unittest.main()
