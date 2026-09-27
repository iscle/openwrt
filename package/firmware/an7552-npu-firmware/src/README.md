# AN7552 NPU firmware

MIT-licensed firmware for the two RISC-V network-processor cores used by
Airoha AN7552/AN7563. This is a new implementation; it does not contain the
Xiaomi NPU binary. The MT7992 radio still requires its separate MediaTek
WM, WA and DSP firmware.

The OpenWrt package builds the bare-metal RISC-V toolchain and installs
`/lib/firmware/airoha/an7552.bin`. No network settings, credentials,
regulatory country, device MAC address or calibration data are compiled in.

For a standalone build:

```
make CROSS_COMPILE=/path/to/riscv32-unknown-elf-
make check
```

The host tests exercise replay counters, packet classification, receive
buffer ownership, station frame validation, transmit completion parsing and
draining pending packets while radio queues are unavailable, per-packet MLO
routing, cross-radio token completion and priority classification.
They use Linux virtual-memory mappings to simulate the reserved workspace.

## Interface

`firmware-abi.h` defines the little-endian image header and ABI version.
The Linux driver checks the header, image bounds, entry point, core count
and each core's runtime ABI announcement before enabling packet queues.
`protocol.h` defines the shared-memory regions and queue sizes. The device
tree must reserve the firmware workspace and packet buffers before boot.

Core 0 submits Wi-Fi transmit descriptors and processes transmit completion.
Core 1 processes receive reordering, replay checks and Ethernet PPE input.
Queue enable/active handshakes transfer ownership between Linux and the
firmware. A separate park handshake stops both cores before driver teardown
or a new firmware load. Cross-core state publication uses RISC-V fences.

The recovery protocol uses fenced trap acknowledgments and per-core readiness for a recovery
interrupt. The host detects exceptions or a stalled heartbeat. If normal
parking times out, NPU timer 0 interrupts both cores; the host still requires
both park acknowledgments before stopping DMA and reusing buffers. A stalled
interconnect transaction can prevent this handshake, in which case recovery
retains the buffers and leaves acceleration stopped. Timer 0 is inside the
existing NPU register aperture; no additional timer driver is involved.

Transmit routing uses host-published DSCP maps and MLO transmit routes. Core 0 selects
an eligible link for each packet using the driver's current TID parity policy.
Both endpoints must retain the same peer identity and the selected BSS/radio;
revoked or reused station identifiers cannot supply a route. Tokens retain
their original TDMA pool identity when the destination radio changes.

Replay protection uses generation-tagged replay domains shared by a peer's MLO links.
Each link retains separate descriptor cookies, and A-MSDU continuation state
is separate for each receive radio. Pairwise-key installation prepares all
changed links, merges their saved counters, and commits receive eligibility
only after all radio keys have been installed. Domain references are released
on key removal and station teardown; stale generations cannot be reused.

ABI 5 requires a negotiated native-bridge receive mode. Each eligible frame gets
an internal C-tag carrying its authenticated receive WCID plus one. The host
PPE driver must use that domain in the lookup, remove it in bound actions,
and invalidate both ingress and egress entries before station reuse. Per-token
saved headers occupy 0x841e0000..0x84210000; CPU misses restore the complete
original header and overwritten descriptor tail. Tagged and unsupported
wireless frames retain the Linux path. Host-ring backpressure must not release
a buffer while PPE still owns it.

This ABI requires a 4 MiB firmware reservation at 0x84000000. Replay domains
occupy 0x84300000..0x8437ffff and the second radio's continuation state occupies
0x84380000..0x843fffff. The packet workspace still starts at 0x84400000.
The host initializes these regions and preserves domain generations across
firmware recovery. Load this ABI only with its matching host driver and DTB.
Host regression tests cover counter sharing, staged activation, removal,
slot reuse and fault boundaries. Hardware qualification remains in progress.

The QoS workspace occupies 0x84290000..0x84297fff, between forwarding work
and station state. Compile-time bounds prevent overlap with those regions.
mac80211 supplies custom QoS maps through an optional driver callback and
restores them when recreating an interface. The radio and NPU receive the
same table derived from cfg80211's classifier. This does not implement a
new adaptive MLO scheduler or negotiated TID-to-link policy.

The fast receive path accepts authorized, individually addressed CCMP-128,
CCMP-256, GCMP-128 and GCMP-256 data with supported IPv4/IPv6 TCP/UDP headers. Unsupported packet layouts and
traffic requiring normal stack processing go to Linux. Replay checks use
per-peer, per-TID counters shared by MLO links using the same key;
a snapshot command revokes the old link entry while
exporting its counters for radio recovery. Counter errors must not fall back
to stale host state.

Packets spanning multiple receive DMA buffers are passed to Linux as a whole.
Continuation buffers contain payload rather than RX descriptors and must not
be parsed independently. Replay validation runs after host reassembly; the
firmware preserves packet boundaries when the host queue is backpressured.

Station authorization and encryption policy are controlled by Linux. A
successful supported pairwise-key installation publishes encrypted TX policy
before forwarding resumes. Key removal revokes forwarding. Learning from an
ordinary CPU data frame can fill an unknown policy but cannot overwrite an
established policy; EAPOL frames never supply data encryption policy.

Transmit key counters belong to the radio hardware. The MT7992 driver,
not this firmware, saves and restores those counters across radio recovery.

## Development

Hardware tests must verify actual delivery and checksums as well as offload
counters. A flow being bound in the PPE is not proof that packets reached
the Wi-Fi client. Test both directions, both bands, IPv4/IPv6, key rotation,
radio recovery and driver teardown. Keep configuration overlays and factory
calibration outside generic firmware artifacts.
