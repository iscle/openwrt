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
	-ffile-prefix-map=$(CURDIR)=. -O2 -g \
	-Wall -Wextra -Werror -ffunction-sections -fdata-sections

# PROF=1: cycle profile in the debugfs status rows 280: and 2c0: (prof.h).
ifeq ($(PROF),1)
CFLAGS += -DNPU_PROF
endif

all: build/an7552.bin

build:
	mkdir -p $@

build/%.o: %.c protocol.h firmware-abi.h native-tx.c.inc station.c.inc tdma.c.inc tx-pool.c.inc txfree.c.inc rro.c.inc ppe.c.inc upsplit.c.inc pn.c.inc pn-cache.h rx-security.c.inc rx-parse.h pkt-view.h prof.h | build
	$(CC) $(CFLAGS) -c $< -o $@

build/start.o: start.S firmware-abi.h | build
	$(CC) $(CFLAGS) -c $< -o $@

build/an7552.elf: build/start.o build/main.o an7552.ld
	$(CC) $(CFLAGS) -nostdlib $(LINKER_FLAGS) -Wl,--gc-sections,--no-relax \
		-Wl,-T,an7552.ld,-Map,build/an7552.map $(filter %.o,$^) -o $@

build/an7552.bin: build/an7552.elf
	$(OBJCOPY) -O binary $< $@

clean:
	rm -rf build

.PHONY: all clean

HOSTCC ?= cc
TESTS := pn pn-cache rx-ownership rx-parse station-frame txfree tx-pool ppe-cache up-pipe up-split

build/test-%: tests/%.c $(wildcard *.h *.c.inc) | build
	$(HOSTCC) -O2 -Wall -Wextra -Werror -pthread $< -o $@

check: $(addprefix build/test-,$(TESTS))
	@set -e; for test in $^; do $$test; done

# Mutation check of the upload pipeline tests (each mutant must fail).
mutate:
	python3 tests/mutate-upsplit.py

.PHONY: check mutate
