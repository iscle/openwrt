/* SPDX-License-Identifier: MIT */
#include "firmware-abi.h"
	.section .fw_header, "a", @progbits
	.long AN7552_FW_MAGIC
	.long AN7552_FW_FORMAT_VERSION
	.long AN7552_FW_ABI_VERSION
	.long AN7552_FW_HEADER_SIZE
	.long __image_size
	.long __entry_offset
	.long AN7552_FW_CORES
	.long 0
	.section .init, "ax", @progbits
	.globl _start
	.option norelax
_start:
	csrwi mie, 0
	csrci mstatus, 8
	fence.i
	fence iorw, iorw
	csrr a0, mhartid
	li t0, 2
	bgeu a0, t0, .Lpark
	la sp, __stack_base
	addi t0, a0, 1
	slli t0, t0, 13
	add sp, sp, t0
	la t0, trap_entry
	csrw mtvec, t0
	call firmware_main
.Lpark:
	wfi
	j .Lpark

	.balign 4
trap_entry:
	/* Report the trap without relying on the stack or C runtime. */
	csrr t0, mhartid
	slli t0, t0, 6
	li t1, 0x3e903000
	add t0, t0, t1
	csrr t1, mcause
	sw t1, 8(t0)
	csrr t1, mepc
	sw t1, 12(t0)
	csrr t1, mtval
	sw t1, 16(t0)
	li t1, 0xdead7552
	fence iorw, iorw
	sw t1, 0(t0)
	j .Lpark
