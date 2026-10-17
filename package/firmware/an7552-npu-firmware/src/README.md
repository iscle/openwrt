# AN7552 NPU firmware

MIT-licensed firmware for the two RISC-V network-processor cores used by
Airoha AN7552/AN7563. This is a new implementation; it does not contain the
Xiaomi NPU binary. The MT7992 radio still requires its separate MediaTek
WM, WA and DSP firmware.

The OpenWrt package builds the bare-metal RISC-V toolchain and installs
`/lib/firmware/airoha/an7552.bin`. No network settings, credentials,
regulatory country, device MAC address or calibration data are compiled in.

## Building

```
make CROSS_COMPILE=/path/to/riscv32-unknown-elf-   # GCC, as the package does
make                                               # clang and lld (LLVM=/usr/bin)
make PROF=1 CROSS_COMPILE=...                      # cycle profile, see below
make check                                         # host tests
make mutate                                        # mutation check of the pipeline tests
make clean
```

Without `CROSS_COMPILE` the Makefile falls back to `$(LLVM)/clang` with
`--target=riscv32-unknown-elf` and `ld.lld`. Both compilers must build the
image; the package uses the GCC toolchain from `riscv32-elf-gcc/host`.

`PROF=1` adds a cycle profile (`prof.h`) that publishes per-section cycle
counts in the debugfs status rows `280:` (core 0) and `2c0:` (core 1). It is
not built by default.

The host tests (`tests/`, built with `HOSTCC`, default `cc`) use Linux
virtual-memory mappings at the NPU addresses to simulate the reserved
workspace:

| test | covers |
| --- | --- |
| `pn` | replay counters: duplicate, carry, TID isolation, key revocation and reset, command ordering |
| `pn-cache` | the batch PN cache gives the results and final table of the uncached checks |
| `rx-ownership` | receive copy, token translation, buffer recycling, refill, empty ring, PPE returns |
| `rx-parse` | fast-path classifier: cipher, error, fragment and control exclusions, truncation |
| `station-frame` | station frame validation: data, EAPOL, VLAN, nested VLAN, truncated headers |
| `txfree` | TXFREE event parsing, host/direct tokens, double completion, the ring copy, no PCIe read for an empty slot |
| `tx-pool` | TDMA ring, buffer pool and radio model: buffer ownership and conservation, refill order, in-flight above the ring size, drops, completion, teardown and restart, a TDMA that ignores the CPU index, a radio ring shared with host frames |
| `ppe-cache` | the PPE index cache re-reads only when the ring looks full and never overruns |
| `up-pipe` | the upload pipeline on one thread, with an ordering contract on every head/tail store |
| `up-split` | the upload pipeline across two harts with a weak-memory model: delivery, back-pressure, key and station changes, RRO stop/restart, buffer conservation |

`make mutate` (`tests/mutate-upsplit.py`) breaks one rule of `upsplit.c.inc`
at a time and requires `up-pipe` or `up-split` to fail for every mutant.

## Interface

`firmware-abi.h` defines the little-endian image header and ABI version.
The Linux driver checks the header, image bounds, entry point, core count
and each core's runtime ABI announcement before enabling packet queues.
`protocol.h` defines the shared-memory regions and queue sizes. The device
tree must reserve the firmware workspace and packet buffers before boot.

Core 0 owns the Ethernet to Wi-Fi direction: it consumes the TDMA ring and
re-arms it from a buffer pool (`tx-pool.c.inc`), submits Wi-Fi transmit
descriptors and processes transmit completion. The
Wi-Fi to Ethernet direction is a two-stage pipeline (`upsplit.c.inc`): core
0 reads the reordered receive descriptors, runs the replay checks and parses
frames; core 1 checks buffer state and station identity, feeds the Ethernet
PPE or Linux, pops the PPE return queue and owns every receive buffer.
Queue enable/active handshakes transfer ownership between Linux and the
firmware. A separate park handshake stops both cores before driver teardown
or a new firmware load. Cross-core state publication uses RISC-V fences.

The fast receive path accepts authorized, individually addressed CCMP data
with supported IPv4/IPv6 TCP/UDP headers. Unsupported packet layouts and
traffic requiring normal stack processing go to Linux. Replay checks use
per-station, per-TID counters; a snapshot command revokes the old entry while
exporting its counters for radio recovery. Counter errors must not fall back
to stale host state.

Transmit key counters belong to the radio hardware. The MT7992 driver,
not this firmware, saves and restores those counters across radio recovery.

## Development

Hardware tests must verify actual delivery and checksums as well as offload
counters. A flow being bound in the PPE is not proof that packets reached
the Wi-Fi client. Test both directions, both bands, IPv4/IPv6, key rotation,
radio recovery and driver teardown. Keep configuration overlays and factory
calibration outside generic firmware artifacts.
