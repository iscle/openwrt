# BE5000 source review

Current base: official OpenWrt `14423144a50d0af747dd3c63e47d86546a426cfb`.
The original consolidation review below used
`5dcf6a3b3da3cf9cfb2a0e73cbeac9a656a58cf6`.
Scope: the cumulative AN7563/BE5000 changes, including bootloader support,
Linux drivers and bindings, mt76/mac80211, bridger, source NPU firmware,
packaging and provisioning documentation. This is a source review, not an
independent security audit or a production qualification.

## Adversarial follow-up to `512d90c50bcc`

This pass reviewed all 98 branch commits since the current upstream base and
compared the supplied Xiaomi 1.0.91 firmware where it provided useful evidence.
Its SHA-256 is
`27968aaa3efa56e7a90a6ef7da7a51f392f612d49b1948d5de20a9304f3e417b`.
The requested REA workflow informed the decomposition and evidence checks;
its engine could not run in this environment because process inspection was
unavailable. The results below come from source inspection, static firmware
analysis and ordinary host tests, not an REA engine run.

The corrective series addresses these confirmed defects:

* Linux and U-Boot BMT now reserve the old data block while saving a remap,
  preventing metadata allocation or stale-table cleanup from destroying the
  only recoverable copy. Linux unmapping holds the same reservation. Linux
  also propagates BMT attachment errors instead of exposing raw NAND, while
  allowing other devices that do not request BMT to retain their raw layout.
* AN7563 restores checksum-error drop controls before marking received packets
  checksum-verified. Stock `fe_core.ko` independently identifies the UDP, TCP
  and IP error-drop bits as 20, 21 and 22.
* Native PPE rejects routed flow rules before parsing them into bridge-only
  entries. Software forwarding remains available. The existing PPE startup
  already flushes SRAM and propagates errors; no duplicate flush is needed.
* NPU core-start ownership is recorded before releasing reset, independently
  of successful startup. Failed starts still require parking; a failed park
  preserves the obligation and quarantines the context.
* mt7996 registration errors unwind initialized DMA, NAPI and deferred work.
  Native NPU drain precedes interrupt masking. Cleanup disables polling,
  waits for poll tails, kills the tasklet, then releases DMA. Removal releases
  both IRQ handlers before freeing their device state.
* AP-VLAN QoS callbacks preserve software maps on non-native platforms. Native
  per-VLAN maps are explicitly rejected before accessing uninitialized driver
  state; the previous policy remains intact.
* The BE5000 exposes its stock-described reset/mesh buttons and rejects flash
  FITs larger than U-Boot's 5-MiB read. The incomplete EVB image profile is
  withdrawn. Unreferenced AN7563 pinmux tables are removed without changing
  active function descriptions.

An independent adversarial reviewer checked the complete candidate. The review
loop found the missing BMT unmap reservation and additional Wi-Fi NAPI-tail and
IRQ-lifetime problems, plus second-device BMT admission; these were corrected
and returned for re-review.
Host regression coverage and a dedicated CI workflow accompany the changes.
See [VALIDATION.md](VALIDATION.md) for what was actually run.

The unwind fix covers errors after DMA setup completes. The pinned upstream
mt76 also has incomplete cleanup for allocation failures inside DMA setup;
that inherited path needs allocation-stage tracking and is not fixed here.

Remaining limitations include NAND data/OOB partial-program behavior on the
actual flash, power-cut testing, live key renewal, warm reset and PCIe platform
ownership. No speculative hardware sequence replaces those validation tasks.
In particular, separate page-data and OOB programming requires device-level
qualification; the BMT reservation tests do not establish NAND compatibility.

## October 19 upstream-main rebase

The complete custom history, including its consolidation merges and inherited
authorship, is replayed onto upstream `14423144a5` (Linux 6.18.55). The source
NPU firmware and its performance paths are retained. Upstream now contains the
selected mt76 revision and the initramfs configuration-stamp fix; duplicate
patches remain removed. The immutable `be5000-test-20261019-r2` tag still names
the preceding build and is not retargeted by this rebase.

Integration changes preserve upstream's new Ethernet DMA map types, queue
flushing, BQL completion locking, RCU queue pointers, MTU calculation and PPE
priority handling. AN7563 TX now records single-buffer mapping ownership and
uses that state to reject duplicate completions. A full or flushing queue is
checked before packet mutation. The AN7563 switch identifier moves past the
new upstream EN7528 identifier. Shutdown now requires NPU quiescence, and an
unresolved AN7563 QDMA idle timeout stops the system before freeing DMA memory
or returning RAM to the bootloader. A terminal admission gate drains setup
and stop operations already in progress and prevents later recovery from
restarting the NPU. The PCI consumer invokes that proof before radio reset;
a dependency on its companion PCI function preserves this shutdown order.

Nested package-selection guards exposed a dnsmasq Kconfig dependency cycle.
The metadata generator now recognizes prerequisites already guaranteed by an
enclosing selection guard, while retaining unrelated prerequisites and virtual
provider selection. The regression includes the actual generated Kconfig with
fatal recursive-dependency checks. The updated ucode API also introduces an
execution-break status: uhttpd now rejects interrupted handler initialization
instead of failing its exhaustive-switch build or accepting partial setup. A separate reviewer checked the metadata
fix and the refreshed Wi-Fi stack. A reviewer independent of the implementation
authors checked Ethernet integration, terminal shutdown and PCI ordering;
confirmed findings were fixed and returned for another review pass.

## October 19 consolidation and performance-port review

Three adversarial reviewers examined the complete custom delta from the original
consolidation base: NPU firmware/transport, Wi-Fi/mac80211, and
platform/storage/networking.
Reviewers then cross-reviewed fixes written by another reviewer. New findings
were fixed and returned for another pass. The performance ports are also
reviewed against ABI 5, rather than treating their original PR tests as proof.

Confirmed issues addressed in this iteration include:

* PPE drain and stop errors previously allowed DMA buffers to be reused.
  Timeout failures now retain ownership, and destructive teardown requires a
  quiescence proof before freeing memory. Unrecoverable teardown fails stopped.
* Failed station/policy handshakes could reuse stale acknowledgments. A failed
  mailbox is quarantined until recovery; later revocations still clear the
  affected authorization and PN eligibility immediately.
* Firmware queues could stop on a sticky error while their heartbeats remained
  healthy. The host now detects those queue faults, including TDMA corruption.
* L1 radio recovery could ignore reset failures and release live TX tokens.
  All recovery phases must succeed before queues and DMA resume, and
  RX/TX polling state is tracked across failed recovery and teardown.
* The mt76 NPU API depended on an Airoha-only kernel header even on other
  targets. A matching local interface and disabled-platform stubs preserve
  those builds.
* NAND remapping ignored failed page copies and could retire the only good
  source. Mapping tables now use verified copy-on-write persistence before
  retiring the old copy. Complete Airoha MTD transactions are serialized so
  lookup, copy, commit and shared scratch cannot race. Ambiguous persistence
  fails stopped; table scan order
  and on-flash format remain compatible with the existing bootloader.
* U-Boot could reject corrected ECC reads, ignore short reads or expose raw
  NAND after BMT attachment failed. Attachment and initial-table persistence
  errors now prevent registration of an incorrectly mapped flash device.
* Ethernet TX timed out and freed DMA-owned buffers. Queue completion now
  owns reclamation; descriptor 255 is valid, and retrying a full queue cannot
  mutate the caller's packet before returning BUSY.
* Live pairwise key replacement lacked a proved old-frame receive drain.
  Duplicate installed keys remain no-ops. Replacement or reused station
  indices are rejected until full radio recovery; removed keys retain a
  tombstone so reinstall cannot reset counters under the same secret.
  This deliberately sacrifices seamless renewal and needs reconnect testing.
* Adding an MLO link could leave its replay state prepared but never active.
  Link addition now commits its prepared state transactionally and propagates
  errors before authorization.
* Splitting upload across cores introduced a PN-update/submission race.
  A fenced pause/active handshake protects both PPE completions and new
  submissions. Receive stop can also finish while a station pause is held.
* The performance port preserves full cookie checks, uncached DMA backing,
  complete-frame handling, write-through shared replay counters and bridge
  domain restoration. A reused receive token starts with a fresh header view.

The accompanying tests execute extracted production functions where practical;
NPU tests compile the actual firmware includes. Failure models cover mailbox,
DMA, memory ordering, NAND persistence, packet bounds and queue ownership.
These are source/model results, not measurements on the router.

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

* **Key replacement:** no live radio/RRO/DMA receive-drain barrier is claimed.
  The conservative rejection/reset guard above prevents that unproved live
  transition. Recovery and reconnection behavior under frequent MLO renewal,
  as well as intermittent discovery failures, need device testing; see
  [NPU.md](NPU.md).
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
