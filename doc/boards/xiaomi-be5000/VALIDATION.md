# Upstream validation

## External 2.5-Gbit Ethernet (2026-09-25)

The missing port required three pieces: MDIO access to the EN8811H at address
15, the PON SerDes in fixed 2500BASE-X Ethernet mode, and GDM2 transmit/receive
routing through QDMA0. Loading the PHY firmware alone did not establish a link.
The register fields were checked against the stock firmware and the AN7552
hardware initialization in [the vendor SDK](https://github.com/lotusmomo/airoha_sdk).
The new PCS implementation uses masked updates to retain unrelated calibration
and reset fields; the existing upstream EN8811H driver is unchanged.

A full build and generic-image audit passed. The RAM candidate passed:

* EN8811H firmware initialization (version 25062302), with both Ethernet MACs.
* 2500-Mbit/s full-duplex negotiation reported by both link partners.
* DHCP from the existing LAN through the bridged 2.5-Gbit port, with the router
  acting only as an AP and management DHCP client.
* Router and upstream gateway pings without loss, and Internet HTTPS explicitly
  bound to the computer's Ethernet interface.
* A 128-MiB transfer in each direction with matching SHA-256 checksums.
* Router interface down/up and computer Ethernet connection restart, followed
  by link recovery and successful forwarding.
* Warm reboot from the RAM candidate back to U-Boot without intervention.
* Both wireless interfaces operational, with existing runtime configuration
  and the ondemand CPU governor retained; no kernel warning or oops observed.

These are functional tests on one unit, not a line-rate throughput benchmark
or long-term endurance test. These initial tests preceded the DSA conversion
described below. Lower negotiated speeds on the external socket were not
validated. Final installed-image checks are recorded in release notes.

## Embedded switch and DSA (2026-09-25)

The MT7530 MMIO driver now owns the AN7563 switch. The MAC exchanges DSA port
metadata through its DMA descriptors and no longer programs switch forwarding
registers. The external EN8811H shares DSA's MDIO bus at address 15: its MDIO
register is inside the switch resource, so a separate platform MDIO controller
would claim overlapping registers.

The first RAM candidate passed a 128-MiB transfer in each direction with matching
SHA-256 checksums through each of the three gigabit sockets. The LAN uplink was
`lan1`; the two computer-facing sockets were `lan2` and `lan3`. All negotiated
1 Gbit/s full duplex. For each computer-facing socket, enabling bridge VLAN
filtering and moving the port from VLAN 1 to isolated VLAN 4093 stopped gateway
reachability; restoring VLAN 1 restored forwarding. These checks exercised DSA
port selection and VLAN isolation, not only interface naming.

The second RAM candidate initialized the EN8811H on the switch MDIO bus with
firmware version 25062302 while retaining all three DSA ports. Both link partners
negotiated 2.5 Gbit/s full duplex. A 128-MiB transfer in each direction matched
the expected SHA-256 checksum, and gateway forwarding passed without packet
loss. The 2.5-Gbit link recovered after an interface down/up cycle, with gateway
forwarding restored. Both radios reached ENABLED state with the runtime
configuration. Final installed-image checks are recorded in release notes.

## Status LED wiring (2026-09-25)

Stock 1.0.53 uses a `pwm-rgb` consumer with a 4-ms period. Its device-tree
child names are misleading: the driver assigns packed brightness bytes in
child order, and the stock `xqled` actions select PWM 4 for orange and PWM 1
for blue. Direct pin tests confirmed active-low operation and both colours;
the owner confirmed the blue colour again with the upstream Airoha PWM driver.
The unchanged upstream driver completed 1,024 duty-cycle updates over both
channels, including fully on and off, without an error. The board uses the
standard `pwm-leds` consumer and OpenWrt diagnostic LED aliases; no custom
LED daemon or direct register writes are installed.

## CPU and wireless validation (2026-09-24)

Official OpenWrt base: `98bc30d154f67557598a2114463e9b97c915463d`.
Kernel: Linux 6.18.52. Test date: 2026-09-24.
Test hardware: one Xiaomi BE5000, 512 MiB RAM.

The full firmware and U-Boot build completed successfully. Four host-side
calibration validation/rejection/no-overwrite tests passed.

The generic RAM candidate passed:

* Normal boot arguments, both CPU cores online, dynamic 500–1000 MHz CPU clock.
* Wired connectivity and SSH through the directly connected Ethernet interface.
* Wi-Fi firmware loading and simultaneous 2.4/5-GHz WPA2 access points.
* 4000 PCI configuration reads concurrent with 20 successful Wi-Fi scans,
  split across both bands and CPUs; native PME remained enabled.
* A preceding image with the same radio/Ethernet changes also passed Wi-Fi
  down/up and reinitialization without kernel warnings.
* Image inspection found no device EEPROM, custom network configuration,
  Wi-Fi country setting, password or SSH host keys.

Release notes record the final image hashes and subsequent installed-image
validation. The baseline branch has its own separately published record.
Country and credentials are supplied only at runtime for temporary tests.
No throughput, DFS, MLO or prolonged endurance claim is made.

## CPU clock transition repair

The stock configuration requests a fixed 1 GHz. On the older Linux 6.12
baseline, a 500-MHz policy request changes the reported target, but the
firmware's actual-frequency query remains at 1 GHz: its power-domain
integration does not apply the requested transition.

The active firmware transition reproduces a crash with both the upstream
kernel and Xiaomi's stock 5.4.55 kernel/CPUfreq driver. In a RAM-backed,
write-protected stock test, 800/1000-MHz cycling stalled after 36 completed
cycles. Steady 800-MHz memory tests passed. Source-switch tests failed at
800 MHz even with CPU 1 offline, without SMC calls, with the temporary PLL
kept enabled, and with increased settling delays.

The vendor source defines a separate MCU CPU divider (BUS_PLL_DIV bits
21:17, encoding 0x0a for divide-by-two). Applying it around each firmware
transition, then restoring the original divider, passed:

* 100 source-switch cycles at 800 MHz and 200 direct PLL transitions.
* 200 transitions through the unmodified firmware SMC routine.
* 1100 firmware transitions across all eleven operating points while running
  20 cross-core, 16-MiB memory-pattern tests (12 rounds each).
* A negative control: removing the divider guard after those successful tests
  reproduced the failure again.
* A full firmware RAM boot with the default ondemand governor, followed by
  1100 CPUfreq sysfs transitions with actual-frequency readback at every step,
  20 memory-test runs, 4000 PCIe configuration reads and 20 Wi-Fi scans.
* Independent per-core timing at 500, 800 and 1000 MHz, plus ondemand
  scaling from 500 MHz idle to 1000 MHz under load and back to 500 MHz.

The driver confines this sequence to AN7563, quiesces the shared cluster with
stop_machine, preserves the firmware's mux fields, restores the divider, and
checks the actual frequency because the vendor setter returns -2 even on
success. No operating point, CPU core, or PCIe PME capability is removed.
The implementation uses the divider encoding from
[the vendor-derived clock source](https://github.com/Ansuel/atf-airoha/blob/94892b9e42eb1f6992e60bce904c3ef42ea995c8/plat/ecnt/en7523/ecnt_cpufreq.c).

These tests establish a working transition sequence on this unit; they do not
establish the underlying silicon erratum or guarantee other board revisions.
The firmware still manages the PLL; this is a Linux-side correction around
its incomplete handoff sequence, not a replacement secure-firmware release.
Final installed-image checks and hashes are recorded in release notes.

## Source cleanup validation

The cleaned series was built with Linux 6.18.52 and U-Boot 2026.07. Both
AN7563 device trees compile without DTC warnings and pass the relevant DT
schemas. The calibration utility's four tests pass. The generated generic
image was checked for an empty root password and absence of custom wireless
settings, SSH host keys and per-device calibration.

A RAM boot of the cleaned kernel with the unit's existing AP configuration
verified DHCP client operation, both radios enabled after the 5-GHz DFS CAC,
the blue status LED, all eleven CPU operating points under ondemand, and a
2500-Mbit/s external Ethernet link. A 128-MiB transfer in each direction over
that Ethernet interface produced matching SHA-256 hashes. No kernel warning
or oops was observed during these checks.

The source-built U-Boot was tested in RAM with NAND identification, existing
BBT/BMT lookup, serial console, Ethernet TFTP and booting the installed
firmware using its default boot command. No bootloader installation was
performed. For this test the raw binary was wrapped in a temporary FIT and
started with `bootm`, which performs the cache/MMU cleanup needed before
entering another bootloader. Direct `go` from a running U-Boot is not an
equivalent entry environment and produced inconsistent NAND failures.

This validation covers one BE5000 unit with the documented replacement flash
layout. It does not validate stock-layout conversion or a complete boot image;
BL2 and BL31 remain external prerequisites.

## Ethernet PPE acceleration

The host-managed PPE candidate uses the AN7552 512-entry SRAM table, initializes
flow learning limits and forwards active-Ethernet traffic directly to GDM2.
It does not enable NPU Wi-Fi acceleration or hardware HTB/ETS queue offload.

RAM tests on the BE5000 passed bidirectional hardware binding between the
2.5-Gbit socket and a gigabit uplink. A 53,111,056-byte HTTPS download matched
its published SHA-256 (`a5d4ba3841e3b52e4ac9854f2cfc1796e3846b14f341d7ea94a987897226c545`).
The tightly sampled transfer increased the CPU softnet processed count by
2,082 packets while the gigabit MAC received 57,438,340 bytes. These measurements
include incidental traffic and are evidence of CPU bypass, not a line-rate
benchmark. Hardware-bound flow entries and their timestamps were also checked.
A second complete download after the flow-manager fixes matched the same hash.
A UDP test received 400 valid DNS replies to 400 requests, with both directions
present as bound UDP flows. IPv6 offload and routed NAT were not hardware-tested.

Moving the 2.5-Gbit port from VLAN 1 to isolated VLAN 100 stopped an established
hardware-offloaded download, cleared the bound entries and blocked new
connections. Restoring VLAN 1 and the original filtering state restored new
connections. Bridger fixes consume command acknowledgements correctly and
invalidate member flows when bridge policy changes. No runtime VLAN changes
are embedded in firmware.

At the Ethernet-only milestone, Xiaomi 1.0.91 NPU firmware was examined
without booting or flashing it. Its mailbox dispatcher at 0x84005d1a accepts SET_WAIT IDs 0–28 and GET_WAIT
IDs 0–7. OpenWrt's current NPU/Wi-Fi integration requires additional buffer,
token and version commands. This binary is not a compatible replacement for
the EN7581 firmware and is not redistributed in this tree. NPU compatibility
required a separately validated legacy interface or suitable vendor firmware.
The subsequent receive-only integration and its validation are described in
[NPU.md](NPU.md).
