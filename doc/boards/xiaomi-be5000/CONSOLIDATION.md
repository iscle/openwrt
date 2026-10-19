# BE5000 branch consolidation — 2026-10-19

`be5000-npu` is the maintained BE5000 branch. PR #2 was merged at
`fac761b8c36683d69f8f0f3b44d67990485d7fc8`, followed by fixes and a merge of
`be5000-upstream` (`d0d9f86778dcefa5b26bae554a8afdeadc91921d`).
The older offload, batching and historical branches are retained through
archival tags rather than maintained as competing firmware branches.

## Selected implementation

The upstream-review branch supplies the newer OpenWrt baseline and ABI 5 NPU
architecture: shared replay counters, bridge isolation, MLO forwarding identity,
key lifecycle, QoS and recovery safeguards. PR #1's compatible receive batching
is ported to it. PR #2 supplies the updated mt76 revision, `-O2` firmware build,
PCIe retraining, reboot DMA shutdown, receive-fragment allocation-failure fix,
and separate UBI overlay. Calibration remains read-only in Linux.

The ABI 1 dual-core upload pipeline, cached packet-view/PN cache and TX buffer
pool are not enabled. They need a separate ABI 5 port and validation; merging
their data structures directly would break the host/firmware interface and
remove security checks. Their original commits remain in repository history.
The independent batching branch duplicates the squashed PR #1 changes.
The old vendor-kernel branch and archived development stages do not supply
additional required code for this source-built port.

The updated mt76 already contains receive-poll shutdown, persistent header
translation policy, link-scoped interface keys and scan completion fixes.
Their older duplicate patches were removed. Station/link identity reservations
and ABI 5 hooks were rebased to the updated station lifecycle.

## PR #2 review fixes

* Preinit no longer formats UBI when attachment fails. Explicit upgrades check
  the partition and attachment before writing firmware, and propagate volume
  creation/removal and configuration-restore errors.
* DMA receive metadata uses uncached access. Reusing a token after radio recovery
  cannot rely on an undocumented FIFO cache-eviction assumption.
* The original split pipeline's epoch-wrap cookie bypass was fixed immediately
  after merging PR #2. The final ABI 5 implementation has no split upload queue
  or truncated-epoch authorization shortcut.

Host regression tests cover upgrade failure paths, replay/key lifecycle,
bridge domains, batching, buffer ownership, MLO routing and fragment allocation
failures. A compiled image is still a hardware test candidate; prior hardware
results in VALIDATION.md do not qualify this merged build. In particular,
verify radio recovery, both bands, VLAN isolation and warm reboot on the device.

## Migration

For an earlier iscle installation using the old single firmware partition,
RAM-boot the matching initramfs, validate the sysupgrade image, then install
with `sysupgrade -n`. See README.md for the replacement bootloader layout,
TFTP addresses and checks. Do not flash the raw U-Boot output or use `-F` to
bypass the layout migration. No device flashing is performed by the build.
