# SPDX-License-Identifier: MIT
LLVM ?= /usr/bin
ifneq ($(CROSS_COMPILE),)
CC := $(CROSS_COMPILE)gcc
OBJCOPY := $(CROSS_COMPILE)objcopy
else
CC := $(LLVM)/clang
OBJCOPY := $(LLVM)/llvm-objcopy
COMPILER_FLAGS := --target=riscv32-unknown-elf
LINKER_FLAGS := -fuse-ld=lld
endif
CFLAGS := $(COMPILER_FLAGS) -march=rv32imc_zicsr_zifencei \
	-mabi=ilp32 -mcmodel=medany -mno-relax -msmall-data-limit=0 \
	-ffreestanding -fno-builtin -fno-pic -fno-pie -fno-strict-aliasing \
	-fno-unwind-tables -fno-asynchronous-unwind-tables \
	-ffile-prefix-map=$(CURDIR)=. -Os -g \
	-Wall -Wextra -Werror -ffunction-sections -fdata-sections

CFLAGS += $(EXTRA_CFLAGS)

all: build/an7552.bin

build:
	mkdir -p $@

build/%.o: %.c protocol.h firmware-abi.h native-tx.c.inc station.c.inc txfree.c.inc rro.c.inc ppe.c.inc pn.c.inc pn-domain.c.inc rx-security.c.inc rx-parse.h tx-priority.h ppe-domain.h | build
	$(CC) $(CFLAGS) -c $< -o $@

build/start.o: start.S firmware-abi.h | build
	$(CC) $(CFLAGS) -c $< -o $@

build/an7552.elf: build/start.o build/main.o an7552.ld
	$(CC) $(CFLAGS) -nostdlib $(LINKER_FLAGS) -Wl,--gc-sections,--no-relax \
		-Wl,-T,an7552.ld,-Map,build/an7552.map $(filter %.o,$^) -o $@

build/an7552.bin: build/an7552.elf
	$(OBJCOPY) -O binary $< $@

.PHONY: all

HOSTCC ?= cc
TESTS := ppe-domain forward-drain mlo-forward pn pn-domain pn-mlo rx-ownership rx-parse station-frame txfree tx-priority

build/test-%: tests/%.c $(wildcard *.h *.c.inc) | build
	$(HOSTCC) -O2 -Wall -Wextra -Werror $< -o $@

check: $(addprefix build/test-,$(TESTS))
	@set -e; for test in $^; do $$test; done

.PHONY: check
