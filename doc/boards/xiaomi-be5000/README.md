# Xiaomi BE5000

This port supports the AN7563/AN7552 BE5000 and its MT7992/MT799A dual-band
radio. The published firmware is generic: calibration comes from each router's
own flash, and the image contains no device MAC, password, country setting or
custom wireless network. OpenWrt generates its standard configuration at first
boot with wireless disabled. Set your actual regulatory country when configuring
wireless.

The three gigabit sockets share `eth0`; the 2.5-Gbit socket is `eth1`. Both
interfaces belong to the default LAN bridge. The external EN8811H uses its
standard Linux PHY driver and firmware, with AN7563 MDIO and 2500BASE-X PCS
drivers. Separate WAN/LAN operation and hardware offload are not validated. The CPU
cluster supports 500–1000 MHz in 50-MHz steps on the upstream branch, using
`ondemand` by default. The AN7563 driver temporarily divides the CPU clock
during the firmware's clock-source handoff, then restores the divider and
verifies the resulting frequency. Both cores and all operating points remain
available. See [VALIDATION.md](VALIDATION.md) for the reproduction and test limits.

The status LED uses hardware PWM: blue on GPIO/PWM 1 and orange on GPIO/PWM 4,
both active-low. Orange indicates boot, failsafe and upgrade; blue stays on when
OpenWrt is ready. This indicates system readiness, not Internet connectivity.
Both colours expose standard LED brightness and trigger controls as
`blue:status` and `orange:status`.

## Build

From this repository's root on an OpenWrt-supported build host:

```sh
cp doc/boards/xiaomi-be5000/feeds.conf feeds.conf
./scripts/feeds update -a
./scripts/feeds install -a
cp doc/boards/xiaomi-be5000/config.buildinfo .config
make defconfig
make -j"$(nproc)"
python3 scripts/tests/test_be5000_calibration.py
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
calibration. Keeping ART as one MTD partition preserves the bootloader's root
partition numbering.

Keep private backups of the complete original flash, stock Factory partition,
bootloader, environment and configuration. The stock Factory data is outside
this layout and can be overwritten during conversion. Never substitute another
router's EEPROM: it includes per-unit calibration and addresses.

### One-time calibration provisioning

`scripts/be5000-calibration.py` validates an original 4-MiB Factory backup,
a 7680-byte EEPROM, or an already prepared 128-KiB calibration block. It rejects
unknown non-erased trailing data. It creates a private output file and does not
write flash:

```sh
python3 scripts/be5000-calibration.py /path/to/own-Factory.bin /private/calibration.bin
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
* The older mt76 baseline backports the unspecified link-ID bounds fix.
  The upstream mt76 baseline already contains this fix.

These changes address failures reproduced on this hardware. Validation on one
router does not establish compatibility with all board revisions, long-term
stability, DFS behavior, MLO, throughput or regulatory certification.
