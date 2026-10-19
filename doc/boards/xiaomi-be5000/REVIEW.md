# BE5000 source review

Base: official OpenWrt `5dcf6a3b3da3cf9cfb2a0e73cbeac9a656a58cf6`.
Scope: the cumulative AN7563/BE5000 changes, including bootloader support,
Linux drivers and bindings, mt76/mac80211, bridger, source NPU firmware,
packaging and provisioning documentation. This is a source review, not an
independent security audit or a production qualification.

## Corrections

* NPU station updates now revoke authorization and routing eligibility on
  either handshake timeout, including a failure after publishing a new
  identity. The regression test exercises the actual driver function with
  modeled MMIO; the pre-fix resume path fails its negative control.
* Firmware rejects obsolete untagged PPE operation and checks the producer
  index before accessing a descriptor. Invalid tokens cannot index packet
  metadata. Host setup leaves receive traffic on the CPU path when the
  matching bridge-domain interface is unavailable.
* Bridger allocates its callback before sending a command, consumes data
  responses until the ACK, and returns send/receive errors. A failed send
  cannot enter an indefinite ACK wait. FDB refresh is marked successful only
  after a successful command. Tests cover data-before-ACK, allocation failure,
  send failure, receive failure and resource release.

* Wireless bindings now describe the Airoha NPU and Ethernet phandles used
  by the radio. Both board device trees and all 14 affected binding schemas
  pass validation.

## Removed or consolidated

* The stock RX-only NPU extraction utility and obsolete integration history.
* Firmware fault-injection hooks, diagnostic PPE packet injection, redundant
  overlap checks and write-only diagnostic state.
* Raw shared-memory debugfs dumps and routine boot-step logging. Named health
  counters, firmware exception information and actionable errors remain.
* Debug commands that reconstruct and rewrite NAND mapping tables, plus
  unrestricted `/dev/mem` access in the AN7563 default kernel configuration.
  Normal bad-block handling remains part of the storage implementation.
* The obsolete standalone MDIO package selection in the build profile.
* The incremental native-PPE patch sequence, consolidated with exact final
  source equivalence before subsequent review changes.

Firmware source and tests use Linux-style formatting. Patch checks retain
only justified exceptions: constant pinmux initializer macros reuse their
arguments, CPU clock handoff needs busy waits inside `stop_machine()`, and
the mt76 `Fixes` commit belongs to the separate mt76 repository. These are
not reasons to rewrite working code solely to silence a style checker.

The documented build installs the feed packages needed by this profile and
their dependencies. Installing every unrelated feed package exposed a Kconfig
dependency cycle in squeezelite with this upstream version; it is not a
BE5000 dependency and no local feed modification is required.

## Remaining review concerns

These are not resolved by rewriting commits or passing host tests:

* **Key replacement:** there is no verified radio/RRO/DMA receive-drain
  barrier for same-index key replacement. A software pause is not equivalent
  to proving that all old-key frames have drained. Frequent MLO renewal and
  intermittent discovery failures remain unqualified; see [NPU.md](NPU.md).
* **Reset and shutdown:** warm reset has stalled before U-Boot. Firmware
  recovery cannot guarantee progress through an interconnect hard lock.
  The safe response is to retain DMA-owned buffers; a cold boot may be needed.
* **PCIe integration:** the AN7563 host initialization still maps fixed SCU,
  chip and PHY addresses and uses a shared one-time initialization flag.
  Resource ownership, multiple host instances, reprobe and power-management
  lifecycle need a proper platform/PHY/reset integration before Linux
  upstream submission. The tested initialization sequence is retained here;
  replacing it without hardware regression coverage would be speculative.
* **NPU platform interface:** this backend deliberately targets the AN7552
  fixed memory map and shares the frame-engine aperture with Ethernet.
  Its ownership contract and long atomic mailbox polling need subsystem
  maintainer review; they are not a generic remoteproc implementation.
* **Boot and storage:** only raw U-Boot is source-built. BL2/BL31 and conversion
  from the stock flash layout remain external prerequisites. The BBT/BMT
  format is vendor-specific; power-loss recovery and worn-flash failure
  injection are not qualified by the existing one-router tests.

Native bridge offload also has explicit capability limits: routed traffic,
tagged Wi-Fi ingress and ambiguous double-tag keys use software; per-flow
PPE packet/byte counters and hardware HTB/ETS scheduling are unavailable.
These limits must remain visible in documentation and driver feature claims.

## Submission and validation

Changes to shared Linux, mt76, mac80211, bridger and U-Boot code need review
by those projects as well as OpenWrt. A clean patch series is not evidence of
acceptance. Preserve inherited authorship and trailers; contributors must
confirm the required real-name Developer Certificate of Origin sign-off
before submitting their own work to maintainers.

Run the source-side checks with:

```sh
python3 -m unittest discover -s scripts/tests -p 'test_*.py'
make -C package/firmware/an7552-npu-firmware/src check
```

Follow [README.md](README.md) for a complete generic image build. Previous
hardware measurements in [VALIDATION.md](VALIDATION.md) apply to their tested
revisions. The router is not flashed as part of this source-cleanup review;
the rebased image requires hardware regression testing before deployment.
