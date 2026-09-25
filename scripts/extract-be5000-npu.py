#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Extract the tested AN7552 NPU firmware from a locally supplied Xiaomi ROM."""

import argparse
import hashlib
from pathlib import Path
import shutil
import subprocess
import sys

ROM_SHA256 = "27968aaa3efa56e7a90a6ef7da7a51f392f612d49b1948d5de20a9304f3e417b"
ROOTFS_OFFSET = 3411972
BLOBS = {
    "npu_rv32.bin": "04d14bea05c3f915813907ec516ae066967f0353ecd933cfefc65fb12f78ce4e",
    "npu_data.bin": "c9f0421ca2ac36854ff45a50dbf34f83fff5d55340b531b3d1a0c2e61fbb8370",
}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("image", type=Path, help="Xiaomi BE5000 1.0.91 ROM")
    parser.add_argument("output", type=Path, help="output firmware directory")
    parser.add_argument("--unsquashfs", help="path to unsquashfs or unsquashfs4")
    args = parser.parse_args()

    if hashlib.sha256(args.image.read_bytes()).hexdigest() != ROM_SHA256:
        raise ValueError("ROM checksum does not match the tested BE5000 1.0.91 image")

    tool = args.unsquashfs or shutil.which("unsquashfs") or shutil.which("unsquashfs4")
    if not tool:
        built_tool = Path(__file__).resolve().parent.parent / "staging_dir/host/bin/unsquashfs4"
        if built_tool.is_file():
            tool = str(built_tool)
    if not tool:
        raise ValueError("install squashfs-tools or specify --unsquashfs")

    # Validate both files before creating any output. No stock configuration or
    # device-specific calibration is extracted.
    blobs = {}
    for source, checksum in BLOBS.items():
        result = subprocess.run(
            [tool, "-cat", "-o", str(ROOTFS_OFFSET), str(args.image), "userfs/" + source],
            check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        )
        if hashlib.sha256(result.stdout).hexdigest() != checksum:
            raise ValueError("extracted firmware checksum mismatch: " + source)
        blobs["an7552_" + source] = result.stdout

    for name, data in blobs.items():
        destination = args.output / name
        if destination.exists() and destination.read_bytes() != data:
            raise ValueError("refusing to replace a different firmware: " + str(destination))
    args.output.mkdir(parents=True, exist_ok=True)
    for name, data in blobs.items():
        (args.output / name).write_bytes(data)
        print(name + ": SHA-256 verified")


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        sys.exit(str(error))
