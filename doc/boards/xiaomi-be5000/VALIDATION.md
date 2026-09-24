# Upstream validation

Official OpenWrt base: `98bc30d154f67557598a2114463e9b97c915463d`.
Kernel: Linux 6.18.52. Test date: 2026-09-24.
Test hardware: one Xiaomi BE5000, 512 MiB RAM.

The full firmware and U-Boot build completed successfully. Four host-side
calibration validation/rejection/no-overwrite tests passed.

The generic RAM candidate passed:

* Normal boot arguments, both CPU cores online, fixed 1-GHz CPU clock.
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

## CPU clock support limit

The stock configuration requests a fixed 1 GHz. On the working Linux 6.12
baseline, requesting 500 MHz changes the policy's reported target to 500 MHz,
but the firmware's actual-frequency query still reports 1 GHz. Its older
power-domain integration never applies those requested transitions.

The Linux 6.18 integration activates the vendor frequency-change SMC, exposing
secondary-core corruption and boot lockups. Tests reproduced the problem with
both cores busy, and with a stop-machine rendezvous. The corresponding traces
and diagnostic code are not included in released images. The port therefore
models the validated fixed clock and does not expose unsupported dynamic
frequency scaling. Both cores and native PCIe PME remain enabled. This support
limit does not claim to repair the vendor firmware's transition defect.
