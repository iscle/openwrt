# AN7552 Wi-Fi receive acceleration

The AN7552 integration uses the NPU for Wi-Fi receive buffering and hardware
reordering, then submits eligible traffic to the host-managed PPE. With `bridger`,
learned Wi-Fi-to-Ethernet bridge flows can bypass the Linux receive/forwarding
path. Traffic addressed to the router and unsupported flows still reaches Linux.

Wi-Fi transmission remains on the normal mt76 path. The tested Xiaomi firmware
has no functional NPU Wi-Fi transmit services. This implementation does not
pretend that those commands succeeded, reserve transmit tokens, or redirect
transmit queues to an inactive engine. This firmware limitation does not prove
that a future independent implementation could not use the hardware differently.
Ethernet-to-Wi-Fi transmission and Wi-Fi-to-Wi-Fi forwarding are not fully
hardware accelerated. Per-flow hardware packet and byte statistics also remain
unavailable; zero counters in the PPE debug files do not mean that a bound flow
is idle.

## Firmware

The tested firmware pair comes from Xiaomi BE5000 ROM 1.0.91. It is not
interchangeable with the EN7581 or AN7583 NPU firmware. The source tree contains
an extractor and driver support, not the proprietary firmware binaries.

Download the [original Xiaomi ROM](https://cdn.cnbj1.fds.api.mi-img.com/xiaoqiang/rom/rd18/miwifi_rd18_firmware_05f43_1.0.91.bin)
and run the following from the repository root before building your local image:

```sh
python3 scripts/extract-be5000-npu.py /path/to/miwifi_rd18_firmware_05f43_1.0.91.bin files/lib/firmware/airoha
make -j"$(nproc)"
```

The extractor requires `unsquashfs`, or the build tree's `unsquashfs4`, and verifies
both the complete ROM and the two extracted files before writing either output.
It does not extract stock settings, boot firmware, MAC addresses or calibration.
The files directory must contain only the intended build additions. Firmware
redistribution rights must be established separately before publishing an image
that contains these vendor blobs.

| File | SHA-256 |
| --- | --- |
| Xiaomi ROM 1.0.91 | `27968aaa3efa56e7a90a6ef7da7a51f392f612d49b1948d5de20a9304f3e417b` |
| `an7552_npu_rv32.bin` | `04d14bea05c3f915813907ec516ae066967f0353ecd933cfefc65fb12f78ce4e` |
| `an7552_npu_data.bin` | `c9f0421ca2ac36854ff45a50dbf34f83fff5d55340b531b3d1a0c2e61fbb8370` |

## Interface differences

The driver accounts for two NPU cores, the packed watchdog control register,
legacy mailbox command limits, the shared RRO CPU-index memory handshake and
13-bit receive descriptor lengths. Unsupported modern transmit/version commands
are not sent. Packet lengths are checked before constructing skbs, and all
fragments must be complete before any buffer ownership transfers to the stack.
Legacy replay, old-packet and duplicate indications are discarded.

The firmware caches host receive-ring addresses for its lifetime. Those two
rings are therefore owned by the NPU and reused across Wi-Fi driver reloads.
Removal quiesces the receive workers and completes firmware cleanup before the
Wi-Fi device releases its packet buffers. It does not restart the cores: doing
so would reset the firmware's TDMA index without resetting the hardware index.
Other queue allocations use the NPU's DMA addressing but are released with the
Wi-Fi device; RRO allocations retain their existing explicit cleanup.

The legacy cleanup command includes at least 200 ms of firmware delays. It uses
a sleepable mailbox wait with a one-second limit, while a protected busy flag
prevents concurrent requests from overwriting its command buffer. Other mailbox
operations retain their atomic wait. If quiescing fails, the driver halts the
cores and prevents NPU reattachment until reboot; a subsequent radio probe uses
the normal mt76 path.

## Validation scope

Initial RAM tests passed 32 MiB transfers in both directions with matching
checksums on both radios. A bridged 30,152,712-byte HTTPS download matched the
original ROM checksum. During the 5 GHz download the PPE contained a bound flow
for the test station, while approximately 3,000 station transmissions produced
96 packets in the host Wi-Fi receive path. This verifies receive offload rather
than association alone.

These measurements are functional checks on one router and client, not maximum
throughput benchmarks. Sustained multi-client load, MLO and full regulatory
certification are not established by them.

Three consecutive driver removal/reattachment cycles, with checksum-verified
32 MiB traffic in both directions between each cycle, passed without kernel
warnings or loss of NPU attachment. The host descriptor-ring addresses remained
unchanged and the TDMA producer/consumer indices stayed synchronized. Both
radios passed again after the reload tests. The timeout failure path was also
exercised: a subsequent probe fell back to normal mt76 and passed checksum
transfers. Release artifacts carry the corresponding installed-image validation
results.
