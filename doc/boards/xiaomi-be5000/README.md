# Xiaomi BE5000

This port supports the AN7563/AN7552 BE5000 and its MT7992/MT799A dual-band
radio. The published firmware is generic: calibration comes from each router's
own flash, and the image contains no device MAC, password, country setting or
custom wireless network. OpenWrt generates its standard configuration at first
boot with wireless disabled. Set your actual regulatory country when configuring
wireless.

The embedded MT7530 switch exposes the three gigabit sockets as `lan1`, `lan2`
and `lan3` through DSA; the 2.5-Gbit socket is `eth1`. All four ports belong to
the default LAN bridge. The external EN8811H uses the switch's MDIO controller,
its standard Linux PHY driver and firmware, and the AN7563 2500BASE-X PCS driver.
DSA manages per-port forwarding, learning and VLAN isolation. The host-managed
PPE supports Ethernet flow offload, including traffic between the gigabit switch
and 2.5-Gbit port. The supplied build profile includes `bridger` to install
hardware bridge flows automatically. The active-Ethernet port uses direct MAC
forwarding; hardware HTB/ETS queue offload is rejected. Per-flow hardware byte
and packet counters are unavailable. Source-built AN7552 NPU firmware enables
Wi-Fi transmit and receive acceleration for supported bridge flows. The build
includes this firmware without extracting Xiaomi NPU binaries. See [NPU.md](NPU.md)
for the implementation, validation and remaining limits. The CPU
cluster supports 500–1000 MHz in 50-MHz steps using
`ondemand` by default. The AN7563 driver temporarily divides the CPU clock
during the firmware's clock-source handoff, then restores the divider and
verifies the resulting frequency. Both cores and all operating points remain
available. See [VALIDATION.md](VALIDATION.md) for the reproduction and test limits.

The status LED uses hardware PWM: blue on GPIO/PWM 1 and orange on GPIO/PWM 4,
both active-low. Orange indicates boot, failsafe and upgrade; blue stays on when
OpenWrt is ready. This indicates system readiness, not Internet connectivity.
Both colours expose standard LED brightness and trigger controls as
`blue:status` and `orange:status`.

See [REVIEW.md](REVIEW.md) for the source-review findings and remaining
upstream-integration concerns. This port is not production-qualified.

## Build

From this repository's root on an OpenWrt-supported build host:

```sh
cp doc/boards/xiaomi-be5000/feeds.conf feeds.conf
./scripts/feeds update -a
./scripts/feeds install luci pciutils
cp doc/boards/xiaomi-be5000/config.buildinfo .config
make defconfig
make -j"$(nproc)"
python3 -m unittest discover -s scripts/tests -p 'test_*.py'
make -C package/firmware/an7552-npu-firmware/src check
```

The build profile enables LuCI explicitly; the hardware device definition
does not force a web interface into minimal images.

The output directory is `bin/targets/airoha/an7563/`. Use the
`xiaomi_be5000-initramfs-kernel.bin` for a RAM-only test and
`xiaomi_be5000-squashfs-sysupgrade.bin` for installation. Preserve the SHA-256
manifest, build configuration and source/feeds revisions alongside the image.

## Boot firmware

The U-Boot package builds a raw `u-boot.bin` from source, with serial and
TFTP recovery. It does not bundle BL2 or BL31. The publicly referenced
Airoha TF-A tree lacks the AN7552 platform files required to rebuild those
stages, and this tree does not redistribute extracted vendor binaries.
The raw U-Boot artifact is not a complete boot image and must not be written
to the boot partition by itself.

Firmware installation below requires the existing compatible bootloader
layout. It preserves the installed boot stages. Stock-layout conversion is
not supplied by this series. Keep the original flash backup and an already
verified recovery path before changing any boot firmware.

## Installation prerequisites

The image targets the replacement AN7563 U-Boot layout below. It is **not** a
stock Xiaomi web-upgrade image. Check the actual partition table and bootloader
before installing; do not flash it to an unmodified stock layout.

| Region | Offset | Size |
| --- | --- | --- |
| U-Boot | `0x000000` | `0x07c000` |
| U-Boot environment | `0x07c000` | `0x004000` |
| ART (including per-device calibration) | `0x080000` | `0x040000` |
| Firmware | `0x0c0000` | `0x4000000` |

The first ART eraseblock is preserved; the second, at `0x0a0000`, holds
calibration. A separate page at `0x0a2000` stores the original wired MAC
addresses, copied from the same unit's stock bdata. Keeping ART as one MTD
partition preserves the bootloader's root partition numbering.

Keep private backups of the complete original flash, stock Factory partition,
bootloader, environment and configuration. The stock Factory data is outside
this layout and can be overwritten during conversion. Never substitute another
router's EEPROM: it includes per-unit calibration and addresses.

### One-time calibration provisioning

`scripts/be5000-calibration.py` validates an original 4-MiB Factory backup,
a 7680-byte EEPROM, or an already prepared 128-KiB calibration block. Supply
the original bdata backup to preserve wired addresses as well. Its 64-KiB
environment must have a valid CRC; only `ethaddr` and `ethaddr_wan2` are copied.
The WAN MAC override, passwords and other environment settings are excluded.
The tool rejects unknown non-erased trailing data and creates a private output
file without writing flash:

```sh
python3 scripts/be5000-calibration.py /path/to/own-Factory.bin /private/calibration.bin \
    --bdata /path/to/own-bdata.bin
```

For the replacement bootloader, the factory block occupies the second 128-KiB
eraseblock of the existing 256-KiB ART partition. The first eraseblock is retained.
**Verify that the destination is erased before provisioning.** If it is not,
stop and identify its contents; never erase unknown data. Back up both blocks.

With UART at 115200 and Ethernet directly connected, stop U-Boot and load the
private file with TFTP to `0x84000000` using addresses appropriate to your local
recovery link. Confirm the transfer is exactly 131072 bytes. `mtd list` must
identify the expected `spi-nand0` logical device and layout. These commands
verify the empty destination, write that block only, and compare the readback:

```text
mtd read spi-nand0 0x84200000 0xa0000 0x20000
mw.b 0x84400000 0xff 0x20000
cmp.b 0x84200000 0x84400000 0x20000
```

Continue only if all 131072 bytes match, and after the TFTP transfer has completed:

```text
mtd write spi-nand0 0x84000000 0xa0000 0x20000
mtd read spi-nand0 0x84200000 0xa0000 0x20000
cmp.b 0x84000000 0x84200000 0x20000
```

Require an exact match. No erase or bootloader/environment write is involved.
The generic Linux device tree exposes this block read-only through NVMEM.
Keep the calibration file private; it is not part of any firmware release.

### Adding wired identities to an existing calibration block

Older provisioned blocks contain only the radio EEPROM. Back up the full ART
partition and verify its checksum before updating one. Prepare a new block
with the matching Factory and bdata backups. Require its EEPROM bytes to match
the installed block exactly and verify that all remaining bytes of the
installed second ART block are erased. If these checks fail, stop; do not erase
or overwrite an unknown layout.

After loading the new private block to `0x84000000`, compare the first `0x2000`
bytes against flash. The following checks also verify that the separate target
page is erased before programming it:

```text
mtd read spi-nand0 0x84200000 0xa0000 0x20000
cmp.b 0x84000000 0x84200000 0x2000
mw.b 0x84400000 0xff 0x800
cmp.b 0x84202000 0x84400000 0x800
```

Continue only after both comparisons match. Write that previously unused page,
then compare the complete block against the prepared file:

```text
mtd write spi-nand0 0x84002000 0xa2000 0x800
mtd read spi-nand0 0x84200000 0xa0000 0x20000
cmp.b 0x84000000 0x84200000 0x20000
```

This does not erase or rewrite the radio EEPROM. The 24-byte `BE5MAC01` record
contains its version magic, two binary MAC addresses and a CRC32; the remainder
of the page is erased padding. The generic device tree reads the MAC cells from
ART offsets `0x22008` and `0x2200e`. A firmware upgrade alone cannot recover
missing factory identities; retain the original backups.

### RAM test and sysupgrade

First TFTP-load the generic initramfs image to `0x84000000` and run
`bootm 0x84000000`. Check Ethernet, the ART calibration cell and Wi-Fi initialization
before writing firmware. Recovery traffic must use the directly attached
Ethernet interface if another network uses the same IP range.

Transfer the matching sysupgrade image to `/tmp/firmware.bin` over that link,
verify its published SHA-256, then validate it:

```sh
sysupgrade -T /tmp/firmware.bin
```

When upgrading from installed OpenWrt, use `sysupgrade /tmp/firmware.bin` to retain
configuration, or `sysupgrade -n /tmp/firmware.bin` for clean generic defaults.
From a RAM boot, supply a previously saved, compatible configuration archive
with `sysupgrade -f /tmp/config-backup.tar.gz /tmp/firmware.bin`, or use `-n`.
Do not preserve the temporary initramfs test settings accidentally.

Keep UART attached for the first installed boot. Verify the calibration checksum,
radio initialization and normal subsequent reboot. Enable Wi-Fi only after
setting your country and choosing your own network names and credentials.

## Fix rationale

* GPIO 6 must be driven high, as the stock bootloader's GPIO initialization does.
  Without it, the Wi-Fi MCU patch-start command times out. The GPIO mapping and
  board hog restore that hardware setup.
* AN7563 PCIe configuration transactions use tag 7, matching the vendor runtime
  path. Tag 0 reproducibly locks the host during concurrent Wi-Fi MMIO and PCI
  configuration reads; changing only the tag resolved this in controlled RAM
  tests. Native PME remains enabled. The upstream kernel already provides the
  physical MSI resource-address fix; this branch does not duplicate that change.
* The newer Ethernet receive buffers include a two-byte alignment offset.
  Enable the AN7563 QDMA receive-offset bit so DMA writes begin at the address
  the networking stack expects. Without it, captured frames lose their first
  two bytes and ARP cannot complete.
* mt7996 failure handling avoids scheduling reset work during initial probe,
  unwinds MCU failure, and stops receive polling before freeing DMA page pools.
* Beacon link fields are snapshotted under RCU before issuing sleeping MCU
  commands; nonblocking TX descriptor construction has an RCU read section.

These changes address failures reproduced on this hardware. Validation on one
router does not establish compatibility with all board revisions, long-term
stability, DFS behavior, MLO, throughput or regulatory certification.
