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
buffer ownership, station frame validation and transmit completion parsing.
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
