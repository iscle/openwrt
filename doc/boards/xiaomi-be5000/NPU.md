# AN7552 Wi-Fi acceleration

This branch builds new MIT-licensed firmware for both AN7552 RISC-V cores.
It replaces the earlier Xiaomi receive-only firmware. No stock NPU extraction
or additional files overlay is needed: the BE5000 device package builds and
installs `an7552-npu-firmware`, including its bare-metal RISC-V host toolchain.
The MT7992 radio and EN8811H Ethernet PHY still require their separate vendor
firmware. This does not replace BL2 or BL31.

Core 0 submits Wi-Fi transmit descriptors and handles transmit completion.
Core 1 handles receive reordering, replay checks and submission to the Ethernet
PPE. Together with `bridger`, this accelerates supported Ethernet-to-Wi-Fi and
Wi-Fi-to-Ethernet bridge flows. Traffic addressed to the router and unsupported
flows still reaches Linux. See the [firmware README](../../../package/firmware/an7552-npu-firmware/src/README.md)
for its versioned ABI, build instructions and host tests.

## Ownership and key lifecycle

The Linux driver validates the image header and runtime ABI before enabling
queues. Queue handshakes transfer DMA ownership; a separate park handshake
stops both cores before shared memory is reinitialized. Reserved workspace and
packet-buffer regions are declared in the device tree.

The receive fast path requires an authorized station, individually addressed
CCMP data and supported IPv4/IPv6 TCP/UDP headers. Other layouts use Linux.
Replay counters are tracked separately for each station and traffic identifier.
Key changes revoke offload eligibility until the key installation succeeds.
Recovery snapshots and revokes receive state rather than using stale host
counters. The MT7992 driver saves hardware transmit packet numbers before a
radio reset, restores them after key installation and checks the readback.

## Validation

The integrated RAM image passed checksum-verified transfers on one BE5000 and
one client. Tests included:

* Simultaneous IPv4 transfers in both directions on each band.
* Repeated pairwise and group key rotations during transfers: 512 MiB each way
  on 2.4 GHz and 1 GiB each way on 5 GHz. Aggregate rates were approximately
  210 and 605 Mbit/s, respectively; these are functional checks, not maxima.
* IPv6 Wi-Fi-to-Ethernet delivery with matching SHA-256 and active offload.
* Full driver reset on 5 GHz with the original client association preserved.
* Actual radio firmware assertions on both bands followed by checksum-verified
  accelerated delivery. The final tests retained the client association, but
  other runs triggered client beacon-loss detection and reassociation.
* Three successive Wi-Fi driver removal/reattachment cycles with the integrated
  NPU backend, without a failed backend or firmware trap.
* Rejection of malformed firmware headers before queue activation, and five
  host tests for replay state, packet parsing, receive ownership, station frames
  and transmit completion parsing.

Installed-image results and exact artifact hashes belong in release notes.
Private runtime configuration and calibration are never release artifacts.

## Limits

Per-flow PPE packet and byte counters are unavailable; zero values in PPE debug
files do not show whether a bound flow is idle. Verify delivery and aggregate
NPU statistics instead. Unsupported packet layouts and ciphers intentionally
use Linux. Hardware HTB/ETS queue offload remains unsupported.

Sustained multi-client load, Wi-Fi-to-Wi-Fi offload, MLO, all cipher combinations,
DFS compliance and regulatory certification are not established by these tests.
Radio recovery has been exercised, but automatic recovery from an NPU firmware
trap is not implemented. Fault paths stop acceleration and may require a reboot.
The firmware and driver are AN7552-specific and are not drop-in replacements
for EN7581 or AN7583 implementations.
