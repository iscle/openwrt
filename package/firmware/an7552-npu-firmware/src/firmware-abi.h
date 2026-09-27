/* SPDX-License-Identifier: MIT */
#ifndef AN7552_FIRMWARE_ABI_H
#define AN7552_FIRMWARE_ABI_H

/* All on-disk fields are little-endian 32-bit words. */
#define AN7552_FW_MAGIC 0x3255504e
#define AN7552_FW_FORMAT_VERSION 1
#define AN7552_FW_ABI_VERSION 5
#define AN7552_FW_HEADER_SIZE 32
#define AN7552_FW_CORES 2
#define AN7552_FW_MAX_SIZE 0x10000

/* Runtime ABI announcement, one word per core in shared SRAM. */
#define AN7552_FW_RUNTIME_ABI 0x1f0

/* One word per core, published after enabling the recovery interrupt. */
#define AN7552_FW_RECOVERY_READY 0x1d0

/* Trap handlers drain stores and acknowledge parking before sleeping. */
#define AN7552_FW_PARK_ACK 0x1e4
#define AN7552_FW_PARK_MAGIC 0x7552a11d
#define AN7552_FW_TRAP_MAGIC 0xdead7552

/* Header: magic, format, ABI, header size, image size, entry offset,
 * core count, reserved (must be zero).
 */
#define AN7552_FW_FORMAT_OFFSET 4
#define AN7552_FW_ABI_OFFSET 8
#define AN7552_FW_HEADER_SIZE_OFFSET 12
#define AN7552_FW_IMAGE_SIZE_OFFSET 16
#define AN7552_FW_ENTRY_OFFSET 20
#define AN7552_FW_CORES_OFFSET 24
#define AN7552_FW_RESERVED_OFFSET 28

#endif
