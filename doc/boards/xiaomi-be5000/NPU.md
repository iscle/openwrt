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
CCMP-128, CCMP-256, GCMP-128 or GCMP-256 data and supported IPv4/IPv6
TCP/UDP headers. Other layouts use Linux.
Replay counters are tracked separately for each station and traffic identifier.
Key changes revoke offload eligibility until the key installation succeeds.
Recovery snapshots and revokes receive state rather than using stale host
counters. The MT7992 driver saves hardware transmit packet numbers before a
radio reset, restores them after key installation and checks the readback.
Existing stations retain their WCIDs across full radio recovery so that bound
hardware flows continue to reference the same peer.

ABI 2 detects NPU exceptions and stalled heartbeats. A timer interrupt can
park a core that is stuck in a busy loop. Recovery requires fenced parking
acknowledgments from both cores, stops DMA, drains outstanding PPE completion
tokens and snapshots replay counters before resetting the radio. Packets that
have not reached a radio queue can be discarded during recovery; buffers
already owned by DMA cannot be reused until the ownership handshake completes.

## Validation

Integrated RAM images passed checksum-verified transfers on one BE5000, a
computer and two additional Wi-Fi clients. Tests included:

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
* Wi-Fi-to-Wi-Fi delivery between external clients and the test computer,
  with hardware-bound flows and CRC-verified transfers in both directions.
* GCMP-128 and GCMP-256 client transfers and full radio resets with the original
  association retained.
* A two-link MLO association using GCMP-256, bidirectional hardware-bound flows
  and a radio reset without reassociation. This does not establish traffic
  distribution across both links.
* Real NPU exceptions and deliberately stalled cores during active transfers.
  Automatic interrupt recovery passed a 1 GiB aggregate transfer with both
  cores stalled, and separate core tests on both bands. Transfer checksums,
  association continuity and restored hardware forwarding were checked.
* Multiple associated stations retaining their hardware identifiers across
  recovery. Before this fix, a changed WCID left an existing hardware flow
  pointing at the wrong station and blocked delivery.
* Rejection of malformed firmware headers before queue activation, and six
  host tests for replay state, packet parsing, receive ownership, station frames,
  transmit completion parsing and pending-packet draining. The drain test
  rejects the previous implementation as a negative control.

Installed-image results and exact artifact hashes belong in release notes.
Private runtime configuration and calibration are never release artifacts.

### Unreleased forwarding changes

ABI 3 supplies per-interface QoS maps and per-station MLO transmit routes.
The source NPU classifies each packet and follows mt76's default/secondary
link policy. This does not implement an adaptive radio scheduler or prove
that the radio transmits simultaneously on both links.

ABI 5 uses AN7563 native bridge entries instead of IP-tuple subflows for
bridging. Their action layout preserves the packet's DS field. The lookup
includes the MAC pair, input port, full VLAN TCI and IP family. Before binding,
the driver checks the complete supported native key against the triggering
packet and checks the rule's input device, VLAN conditions and protocol.

Wi-Fi inputs otherwise share hardware source port 7. The source NPU inserts
an internal C-tag containing the authenticated receive WCID plus one, giving
each station a separate hardware lookup domain. Bound actions remove that
tag. A miss restores the original receive buffer, including the descriptor
bytes preceding the Ethernet header, before delivery to Linux. This mode
requires matching firmware, transport, mt76 and Ethernet drivers.

Station removal revokes receive authorization, waits for earlier receive
callbacks, then invalidates both source-domain entries and entries targeting
the station before its WCID can be reused. Device removal prevents further
binding and releases references even when a TC rule remains installed. Each
hardware slot has an explicit software owner; a stale rule cannot erase a
replacement flow. Failed table writes disable acceleration before ownership
is released. Competing MAC-only TC parents are rejected rather than replacing
a live owner's actions.

The supported wired input formats are untagged IPv4/IPv6 and a single
802.1Q or 802.1ad tag. VLAN actions preserve the requested output stack and
priority. Wi-Fi fast-path input remains complete, untagged unicast IP traffic
accepted by the existing receive/security classifier. Unsupported frames use
Linux. In particular, double-tag input TPIDs and some non-IP EtherTypes alias
in this hardware's native key, so they are not bound. Routed traffic also
uses Linux: the IP-tuple classifier cannot carry the private ingress domain.
These are deliberate capability boundaries, not claims of unrestricted
hardware forwarding.

Candidate validation includes 401,408 firmware tag/restore combinations and
202,112 actual kernel matcher checks. A RAM test forwarded 64 packets through
an active station domain while 128 otherwise matching packets with different
domain IDs produced hardware misses and no endpoint leakage. With the bridge
manager frozen, station removal still invalidated both forwarding directions;
the phone then rejoined and its TCP test completed.

In the RAM candidate, wired 802.1Q, 802.1ad and IPv6 tests each passed 1,000
bidirectional integrity-checked UDP exchanges, preserving all 256 DS-field
values with native entries bound. Mismatched VLAN, input-port, TPID, priority
and tag-depth rules stayed unbound. Double-tag traffic passed through Linux.
The host received 44,651 Wi-Fi upload datagrams with DS field 160 intact.
A sustained download followed six changes between DS fields 0 and 160 at the
NPU transmit buffers. These results qualify those paths, not general Wi-Fi
recovery, frequent MLO rekeying or a production release. Gigabit ingress also
binds native entries with the corrected five-bit source port.

The bridge manager must advance its software idle counter before querying
hardware activity. Otherwise a fresh hardware last-use timestamp is immediately
aged, and drivers without packet counters cannot refresh active FDB entries.
The update also stops accessing a flow after its idle deletion. Tests of the
actual update function reproduce both failures before the fix and pass with it.

With the corrected bridge manager, a six-minute bidirectional IPv4 test with
30-second FDB aging transferred 4,934,205,440 bytes toward the phone and
15,069,872,128 bytes back, averaging 109.65 and 334.88 Mbit/s. Every one-second
interval made progress in both directions. Native entries remained bound,
aggregation sessions stayed unchanged, and the NPU reported zero faults or
recoveries. A five-second sample measured 9.56% total CPU activity.
An IPv6 bidirectional test then passed for 20 seconds at 112.82 and 343.33 Mbit/s
with native entries in both directions. Its first connection attempt timed out
during neighbor discovery; the retry passed after a router-originated IPv6
reachability check. Another wireless client answered all 200 IPv6 probes.
These are bounded regression results, not production or MLO-rekey qualification.

The generic image passed the empty-password/no-private-settings audit and the
router's sysupgrade validation. Flashing completed on September 29; the ensuing
warm reset stalled before U-Boot. A cold boot succeeded. Readback of the complete FIT and used squashfs matched
the tested image. The packaged NPU firmware and bridge manager matched their
builds, and the bootloader and environment were unchanged. Calibration matched
the September 28 backup after the earlier wired-MAC correction. Normal AP
configuration was restored byte-for-byte; DHCP-client management, disabled
client DHCP service, both radios and the 2.5G link were verified. Normal client
traffic populated native hardware entries after installation.

Earlier RAM tests used MediaTek's July MT7992 radio binaries. RAM images now
use the exact generic package's March binaries by default; the July overlay
requires an explicit test option. A 120-second bidirectional TCP regression
passed with the packaged binaries, after an initial connection attempt failed
before traffic started. This is short regression coverage, not stability or
release qualification.
A later packaged-firmware MLO association accepted unicast ARP but did not
answer broadcast ARP. Cached unicast traffic still passed. Reloading with
frequent group-key renewal restored broadcast reachability, but changed other
association state too. A subsequent fresh 5 GHz association passed twelve
broadcast-ARP checks before any renewal. The failure is intermittent and its
cause is not yet isolated. Unicast ARP seeding was used only to separate a
forwarding regression test from this issue.
These changes have not been qualified for a production release.

## Limits

Per-flow PPE packet and byte counters are unavailable; zero values in PPE debug
files do not show whether a bound flow is idle. Verify delivery and aggregate
NPU statistics instead. Unsupported packet layouts and ciphers intentionally
use Linux. Hardware HTB/ETS queue offload remains unsupported.

Long-duration multi-client load, dynamic MLO link selection, cross-link replay
rejection, all cipher combinations, DFS compliance and regulatory certification
are not established by these tests. CCMP-256 has host-test coverage but no
successful hardware-client validation. Basic two-link MLO transfer and reset
results are not proof of full simultaneous-link performance.

Pairwise-key updates now prepare replay snapshots and suspend all changing
links before programming any radio key. Accelerated forwarding resumes only
after every installation succeeds; an error revokes all affected links.
Identical key objects retain their hardware packet numbers and replay state.
Thirteen tests using the actual driver functions cover update ordering,
partial failures, replay restoration, duplicate keys and single-link operation.
The previous implementation fails the ordering test. A 180-second single-link
SAE/CCMP test passed with pairwise renewal every 30 seconds and group renewal
every 45 seconds. That transfer terminated at the router, so it does not
qualify PPE forwarding. A separate MLO transfer failed during group renewal;
the transaction change alone does not resolve that failure.

The interface-key iterator now filters keys by link ID when restoring or
removing a link. Previously, a partner link's group key could overwrite the
target link's key at the same index. Link teardown also removes keys instead
of reinstalling them. Two additional actual-function tests cover link-scoped
group keys and non-MLO compatibility; the old iterator fails the negative
control. With these fixes, a 120-second MLO bidirectional transfer completed
through a forced radio restart without reassociation. Broadcast ARP also
worked after clearing the router's neighbor entry. This is recovery regression
coverage, not proof that the intermittent renewal and broadcast failures are
resolved.
A 180-second MLO transfer also completed with group renewal every 15 seconds
and pairwise renewal held at one hour. The post-transfer broadcast-ARP check
failed, then later recovered during another transfer. Group-handshake success
therefore does not establish reliable broadcast delivery.
A separate 180-second test with pairwise renewal every 30 seconds and group
renewal every 45 seconds completed without reassociation or NPU faults. This
short pass does not rule out the earlier intermittent failures.

Receive validation now clears an incomplete A-MSDU's continuation state before
checking a new MPDU's packet number. Previously, rejecting a replayed first
subframe could leave the old aggregate eligible to accept following subframes.
The firmware receive test reproduces that acceptance with the old code and
rejects it with the fix. The full firmware host suite and OpenWrt build pass.
In RAM, a 120-second bidirectional transfer completed through radio recovery,
followed by a successful 30-second fresh transfer. The original association
survived and NPU faults stayed at zero. The initial connection failed because
of broadcast ARP; these transfers used diagnostic unicast ARP seeding and do
not qualify ordinary connectivity.

The RX link lookup now returns no station when the reported band has no
matching VIF or peer link, instead of falling back to a WCID on another band.
An actual-function test covers link removal and restoration and reproduces
the old fallback. The full build passes; live dynamic-link coverage remains
outstanding.

ABI 4 adds common MLD replay-state ownership with generation-tagged domains,
per-radio aggregate continuation and staged activation across affected links.
All ten firmware host-test binaries and sixteen actual-driver transaction
scenarios pass. The integrated image builds and its firmware ABI and actual
DTB reservation pass the generic-image audit. RAM tests passed a two-minute
bidirectional transfer and a three-minute transfer through forced radio
recovery, preserving the association. A read-only hardware snapshot confirmed
that both link WCIDs reference one domain, with a new generation and preserved
counters after recovery.

An independent over-the-air capture also checked broadcast encryption across
an idle radio recovery. All 214 complete captured broadcast frames authenticated
with the test network's GCMP-256 group keys. On 5 GHz, the packet number advanced
from 5151 before reset to 5154 afterward, with no sampled sequence-number rollback.
The phone stopped answering ARP and reassociated about 28 seconds after reset;
this run did not preserve its connection. Correct broadcast encryption therefore
does not establish transparent recovery or rule out receive-path and client-state
problems. Subsequent reassociations used only one link and are not two-link MLO
qualification results.

A subsequent single-link capture included the client's link address and AP
beacons. The client retried null frames during the reset and transmitted a
protected deauthentication frame at approximately 2.95 seconds. AP beacons
resumed at approximately 3.73 seconds, after the client had left. The beacon
TSF also restarted, but the deauthentication preceded the first new beacon.
This separates that forced-reset outage from a broadcast encryption failure;
it does not explain every earlier discovery failure or the rekey failures.

After a fresh two-link association, a 120-second bidirectional TCP test passed
at 180.06 Mbit/s toward the phone and 372.85 Mbit/s from it. A following
600-second run passed at 155.74 and 380.65 Mbit/s respectively, carrying
11,681,529,856 and 28,551,020,544 received bytes. Both links and the original
association remained present, with zero NPU faults or recoveries. Clearing the
router's neighbor entry after each run still allowed address discovery and
three successful pings. No unicast ARP seeding was used. A five-second sample
during the longer run measured 6.02% total CPU activity with bound PPE flows.
These runs used unchanged one-hour key timers; they do not qualify frequent
rekeying, simultaneous transmission on both links, or peak radio throughput.

These are short regression results. A planned ten-minute PTK30/GTK45 stress
test lost its association at a pairwise renewal after roughly three minutes of
traffic. Broadcast discovery also failed after the otherwise successful radio
recovery. ABI 4 does not resolve those failures or qualify a production release.
It requires the matching 4 MiB reserved-memory region, firmware and host driver. A verified hardware
receive-drain barrier during same-index key replacement remains incomplete. Frequent MLO pairwise-key renewal also failed
intermittently with a Xiaomi 15 on HyperOS 3.0.304.0.WOCEUXM, including with
NPU offload disabled. Captured handshake authentication and key-installation
timing implicate an early client key switch, but this is not a verified
client-firmware fix or a production qualification result.

Automatic recovery covers core exceptions and interruptible busy loops. A
blocked interconnect transaction may prevent safe parking; that failure leaves
acceleration stopped and can require a power cycle.
The firmware and driver are AN7552-specific and are not drop-in replacements
for EN7581 or AN7583 implementations.

### Recovery validation

An MLO client completed 192 MiB of bidirectional, integrity-checked TCP
traffic while both NPU cores were deliberately stalled in a private RAM
image. Automatic recovery retained the association, TCP connection, station
indices, BSS indices and preferred link. The interrupted 32 MiB upload took
8.15 seconds; subsequent uploads returned to 375–386 Mbit/s, with PPE
entries bound in both directions. A separate single-link test transferred
256 MiB in both directions through the same recovery path.

This tests recovery from a software core stall, not uninterrupted service:
restarting the radio still pauses traffic. Cross-link replay rejection,
dynamic MLO link changes and simultaneous use of both links remain separate
validation requirements.

### Priority-controlled MLO receive offload

The receive descriptor's WCID is not necessarily tied to the physical
receive band. A Xiaomi 15 delivered TID 5 traffic using its secondary WCID
on both bands. The host now explicitly marks authorized MLO station
entries, allowing that mapping while retaining source-address, decryption
and packet-number checks. Single-link stations still require a matching
receive band.

In a 20-second TCP upload with TOS 160, this removed CPU fallback and
increased receiver throughput from 129 to 461 Mbit/s. Aggregate router CPU
utilization fell from 47.7% to 8.0%. A separate reverse run reported
740 Mbit/s at the sender and 6.6% router CPU. Both runs had PPE entries
bound in both directions. These are measurements on one client and RF
setup, not a throughput guarantee or proof of simultaneous RF transmission.
