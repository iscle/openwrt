# Baseline validation

Source base: xinyooo/openwrt `v2026.06-r1`, commit
`eea04afbf5df16057101ab7a749d6119336bb303` (Linux 6.12.85).
Test hardware: one Xiaomi BE5000, 512 MiB RAM.

The functional fixes were tested with kernel lock diagnostics, repeated radio
probe/removal on both CPUs, native PME with MSI and legacy INTx, concurrent
PCI configuration reads and Wi-Fi scans, and simultaneous 2.4/5-GHz WPA2 APs.
A client connected to both APs and loaded the router's web interface.
That build was installed and verified by flash readback and normal NAND boot.

The generic NVMEM-based image was separately RAM-booted on 2026-09-24:

* Its ART calibration cell matched the router's own prepared Factory data.
* The preceding preserved ART eraseblock was unchanged, and the original
  MTD partition numbering was retained.
* Both bands initialized automatically, with native PME enabled.
* 4000 PCI configuration reads and ten concurrent scans completed successfully.
* Both temporary WPA2 APs started on 2.4 and 5 GHz without kernel warnings.
* Test wireless settings were removed and the defaults returned to disabled.
* The host calibration tool passed validation/rejection/no-overwrite tests.

Country and credentials were supplied only at runtime for testing and are not
included in the firmware or repository. The generic image does not include
per-device EEPROM bytes. No throughput, DFS, MLO or prolonged endurance claim
is made. Upstream-branch results are recorded separately on that branch.
