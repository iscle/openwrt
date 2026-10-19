#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Exercise nested selection guards through the real metadata generator.

Set KCONFIG_CONF to scripts/config/conf to also run the generated Kconfig.
"""

import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "scripts/package-metadata.pl"


def package(name, depends="", extra=""):
    return (f"Source-Makefile: package/{name}/Makefile\n"
            f"Package: {name}\nTitle: {name}\nCategory: Test\n"
            f"Depends: {depends}\n{extra}\nDescription: {name}\n@@\n")


def stanza(config, name):
    return re.search(r"\tconfig PACKAGE_" + re.escape(name) +
                     r"\n(.*?)(?=\n\tconfig |\nendmenu)", config, re.S)[1]


class PackageMetadataTest(unittest.TestCase):
    def generate(self, metadata, directory):
        directory = Path(directory)
        info = directory / "packageinfo"
        info.write_text(metadata)
        return subprocess.run(
            shlex.split(os.environ.get("PERL", "perl")) +
            [str(SCRIPT), "config", str(info)], cwd=directory,
            check=True, text=True, capture_output=True).stdout

    def fixture(self, guard="PACKAGE_consumer_feature", extra=""):
        return (package("consumer", f"+{guard}:middle") +
                package("middle", "+IPV6:ipv6 +!LINUX_6_12:newkernel") +
                package("ipv6", "@IPV6 " + extra) +
                package("newkernel", "@!LINUX_6_12"))

    def test_nested_positive_and_negative_guards(self):
        with tempfile.TemporaryDirectory() as directory:
            config = self.generate(self.fixture(), directory)
        consumer = stanza(config, "consumer")
        self.assertNotIn("depends on", consumer)
        self.assertIn("select PACKAGE_middle if PACKAGE_consumer_feature", consumer)
        self.assertIn("depends on IPV6", stanza(config, "ipv6"))
        self.assertIn("depends on !LINUX_6_12", stanza(config, "newkernel"))

    def test_real_prerequisite_is_preserved(self):
        with tempfile.TemporaryDirectory() as directory:
            config = self.generate(self.fixture("FEATURE", "@NEEDED"), directory)
        self.assertIn("depends on !(FEATURE && IPV6) || NEEDED",
                      stanza(config, "consumer"))

    def test_disjunct_is_not_an_enclosing_guard(self):
        metadata = (package("consumer", "+FEATURE:middle") +
                    package("middle", "+IPV6||OTHER:ipv6") +
                    package("ipv6", "@IPV6"))
        with tempfile.TemporaryDirectory() as directory:
            config = self.generate(metadata, directory)
        # Preserve existing expression emission; a disjunct by itself must
        # not be treated as the original selection guard.
        self.assertIn("depends on !(FEATURE && IPV6||OTHER) || IPV6",
                      stanza(config, "consumer"))

    def test_virtual_provider_rank_and_requirements_are_preserved(self):
        metadata = (package("consumer", "+FEATURE:virtual") +
                    package("primary", "@ARCH", "Provides: virtual\nDefault-Variant: 1") +
                    package("alternate", "", "Provides: virtual"))
        with tempfile.TemporaryDirectory() as directory:
            config = self.generate(metadata, directory)
        consumer = stanza(config, "consumer")
        self.assertIn("select PACKAGE_primary if FEATURE && PACKAGE_alternate<PACKAGE_consumer",
                      consumer)
        self.assertIn("depends on !(FEATURE && !PACKAGE_alternate) || ARCH", consumer)

    def test_generated_kconfig_accepts_package_owned_feature(self):
        conf = Path(os.environ.get("KCONFIG_CONF", ROOT / "scripts/config/conf"))
        if not conf.is_file():
            self.skipTest("build scripts/config/conf or set KCONFIG_CONF")
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            image_config = directory / "package/test/image-config.in"
            image_config.parent.mkdir(parents=True)
            image_config.touch()
            config = self.generate(self.fixture(), directory)
            (directory / "Kconfig").write_text(
                'mainmenu "Nested guards"\n'
                'config MODULES\n\tbool\n\tmodules\n\tdefault y\n'
                'config IPV6\n\tbool "IPv6"\n'
                'config LINUX_6_12\n\tbool "Old kernel"\n' + config +
                'if PACKAGE_consumer\n'
                'config PACKAGE_consumer_feature\n\tbool "Feature"\n'
                'endif\n')
            for ipv6, old_kernel in (("y", "n"), ("n", "y")):
                with self.subTest(ipv6=ipv6, old_kernel=old_kernel):
                    seed = directory / "seed"
                    seed.write_text('CONFIG_MODULES=y\nCONFIG_PACKAGE_consumer=y\n'
                                    'CONFIG_PACKAGE_consumer_feature=y\n'
                                    f'CONFIG_IPV6={ipv6}\nCONFIG_LINUX_6_12={old_kernel}\n')
                    result = subprocess.run(
                        [str(conf.resolve()), "--fatalrecursive", f"--defconfig={seed}", "Kconfig"],
                        cwd=directory, text=True, capture_output=True,
                        env={**os.environ, "KCONFIG_CONFIG": str(directory / ".config")})
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                    generated = (directory / ".config").read_text()
                    self.assertIn("CONFIG_PACKAGE_consumer=y", generated)
                    self.assertIn("CONFIG_PACKAGE_consumer_feature=y", generated)
                    self.assertIn("CONFIG_PACKAGE_middle=y", generated)
                    for name, enabled in (("ipv6", ipv6 == "y"),
                                          ("newkernel", old_kernel == "n")):
                        if enabled:
                            self.assertIn(f"CONFIG_PACKAGE_{name}=y", generated)
                        else:
                            self.assertNotRegex(generated, f"CONFIG_PACKAGE_{name}=[ym]")


if __name__ == "__main__":
    unittest.main()
