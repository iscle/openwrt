/* SPDX-License-Identifier: MIT */
OUTPUT_ARCH(riscv)
ENTRY(_start)
MEMORY {
	CODE (rx) : ORIGIN = 0x84000000, LENGTH = 0x10000
	LOCAL (rw) : ORIGIN = 0x3e900000, LENGTH = 0x3000
	STACK (rw) : ORIGIN = 0x84100000, LENGTH = 0x4000
}
SECTIONS {
	.fw_header : { __image_start = .; KEEP(*(.fw_header)) } > CODE
	.init : { KEEP(*(.init)) } > CODE
	.text : { *(.text .text.*) } > CODE
	.rodata : { *(.rodata .rodata.*) } > CODE
	__image_size = . - __image_start;
	__entry_offset = _start - __image_start;
	.data : { *(.data .data.* .sdata .sdata.*) } > LOCAL
	.bss (NOLOAD) : { *(.bss .bss.* .sbss .sbss.* COMMON) } > LOCAL
	.stacks (NOLOAD) : { __stack_base = .; . += 0x4000; } > STACK
	/DISCARD/ : { *(.comment .note.* .eh_frame*) }
}
/* The firmware intentionally has no runtime-initialized global state. */
ASSERT(SIZEOF(.data) == 0, "unexpected initialized data")
ASSERT(SIZEOF(.bss) == 0, "unexpected BSS")

ASSERT(SIZEOF(.fw_header) == 32, "invalid firmware header size")
