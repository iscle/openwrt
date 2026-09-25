# AN7552 Wi-Fi acceleration

The BE5000 package builds MIT-licensed firmware for both AN7552 RISC-V cores.
It includes no Xiaomi NPU binary. The MT7992 radio and EN8811H Ethernet PHY
still use their separate vendor firmware. BL2 and BL31 are not supplied.
See the [firmware README](../../../package/firmware/an7552-npu-firmware/src/README.md)
for the image format, memory layout, standalone build and regression tests.

## Packet paths

Core 0 submits Wi-Fi transmit descriptors and handles transmit completion.
Core 1 handles receive reordering, replay protection and Ethernet PPE input.
The Linux drivers and `bridger` install supported bridge flows. Traffic for
the router and unsupported forwarding rules remain on the CPU path.

AN7563 native bridge entries preserve the IP DS field. Their lookup includes
the MAC pair, input port, full outer VLAN TCI and IP family. The driver checks
both the complete native key and the Linux rule before binding a flow.
Wired ingress supports untagged IP and one 802.1Q or 802.1ad tag. Double-tag
input, unsupported EtherTypes and routed flows remain in software because
the hardware key cannot represent their complete ingress identity.

Wi-Fi ingress shares hardware source port 7. The NPU inserts an internal
C-tag carrying the authenticated receive WCID plus one, separating stations
into distinct lookup domains. Bound actions remove the tag. On a miss or
CPU-ring backpressure, firmware retains DMA ownership and restores the exact
original receive buffer before delivery to Linux. Tagged Wi-Fi input uses
the CPU path. The firmware rejects the former untagged PPE mode.

The receive fast path accepts only authorized, individually addressed,
complete IPv4/IPv6 TCP/UDP frames with supported CCMP or GCMP protection.
Control traffic, EAPOL, malformed headers and unsupported packet layouts use
Linux. Per-interface DSCP maps use cfg80211 classification. MLO transmit
selection follows mt76's default/secondary-link TID policy; it is not an
adaptive link scheduler.

## Lifetime and isolation

The host validates the firmware header, bounds, entry point and both cores'
runtime ABI before activating queues. ABI 5 requires matching firmware,
transport, mt76 and Ethernet drivers, including a 4-MiB reserved workspace.
Queue handshakes transfer DMA ownership. A separate park handshake stops
both cores before shared memory may be reused.

Station removal revokes receive authorization, waits for earlier receive
callbacks and invalidates flows both sourced from and targeting that station
before its WCID can be reused. Device removal blocks new bindings. Hardware
slots have explicit software owners so a stale rule cannot delete a replacement
flow. Failed table writes disable acceleration before releasing ownership.

MLO links share generation-tagged replay domains, with independent aggregate
continuation state for each receive radio. Key updates suspend the affected
links and prepare their replay state before installing new radio keys. Receive
eligibility is committed only after all installations succeed; failures revoke
all affected links. Duplicate key installation retains existing packet numbers.
The radio driver snapshots hardware transmit packet numbers before recovery,
restores them after key installation and verifies the readback. Recovery keeps
station and BSS identifiers stable for existing hardware flows.

The driver detects exceptions and stalled heartbeats. A recovery interrupt
can park an otherwise unresponsive software loop. Both cores must acknowledge
parking before DMA teardown and buffer reuse. Pending packets may be dropped;
DMA-owned buffers must be retained until the ownership handshake completes.
A blocked interconnect transaction can prevent parking and require a power
cycle. Debugfs exposes health information, not writable firmware commands.

## Validation scope

The firmware host suite tests packet parsing, replay protection, shared MLO
replay domains, station authorization, queue ownership, backpressure, token
completion, QoS routing and exact bridge-tag restoration. It includes 401,408
bridge-domain transformations and malformed-input rejection. The kernel
matcher was separately exercised with 202,112 input combinations. Host tests
cannot establish hardware DMA ordering or radio interoperability.

Before this source cleanup, tests on one BE5000 established:

* Checksum-verified traffic on both bands and both wired port types, IPv4/IPv6,
  Wi-Fi-to-Wi-Fi forwarding and hardware-bound flows in both directions.
* Station removal invalidation with the flow manager stopped, and isolation
  between valid and incorrect internal station domains.
* Wired 802.1Q/802.1ad forwarding and preservation of all 256 DS-field values;
  ambiguous tag stacks stayed in software.
* A six-minute bidirectional run with 30-second bridge FDB aging, continuous
  progress in both directions and zero NPU faults. This reproduced and then
  verified the correction to bridger's hardware-last-use handling.
* Two-link MLO association and a ten-minute bidirectional transfer with normal
  key timers, successful discovery afterward and no NPU faults. Association
  on two links does not prove simultaneous radio transmission on both.
* CCMP-128, GCMP-128 and GCMP-256 client transfers, plus software-core-stall and
  radio-restart recovery tests. Some forced radio resets caused reassociation.

Those results apply to the tested artifacts, not automatically to a rebased
or cleaned build. Exact release hashes belong with the release artifacts.
Temporary credentials, captures, calibration and configuration stay outside
the generic source and firmware images. This series is not production-qualified.

## Remaining implementation and qualification work

* A verified hardware receive-drain barrier for same-index key replacement
  is incomplete. Suspending software forwarding alone does not prove that
  all old-key frames have left the radio, reorder engine and DMA queues.
* Frequent MLO pairwise renewal failed intermittently, including with NPU
  offload disabled. Captures suggest an early client key switch, but do not
  establish a client-only cause or a verified fix.
* Broadcast ARP and IPv6 discovery have intermittently failed even when cached
  unicast traffic worked. Their cause is unresolved. Unicast neighbor seeding
  used during diagnosis is not an acceptable production remedy.
* Warm reset can stall before U-Boot. Interrupt-assisted NPU recovery also
  cannot recover every interconnect hard lock. Neither is fixed by cleanup.
* Dynamic MLO link changes, cross-link replay behavior on real hardware,
  CCMP-256 client operation, long-duration multi-client stress and DFS behavior
  require further validation. No regulatory certification claim is made.

Per-flow PPE byte/packet counters and hardware HTB/ETS scheduling are not
implemented. Unsupported routed, tagged-Wi-Fi and double-tag paths consume
CPU resources. Hardware-bound entries and aggregate counters alone are not
proof of delivery; verify both endpoints and payload integrity.
