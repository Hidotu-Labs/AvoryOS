.SUFFIXES:

override OUTPUT := kernel

ARCH := x86_64
PREFIX := /usr/local

ifeq ($(filter $(ARCH),aarch64 loongarch64 riscv64 x86_64),)
    $(error Architecture $(ARCH) not supported)
endif

TOOLCHAIN :=
TOOLCHAIN_PREFIX :=
ifneq ($(TOOLCHAIN),)
    ifeq ($(TOOLCHAIN_PREFIX),)
        TOOLCHAIN_PREFIX := $(TOOLCHAIN)-
    endif
endif

ifneq ($(TOOLCHAIN_PREFIX),)
    CC := $(TOOLCHAIN_PREFIX)gcc
else
    CC := cc
endif

LD := $(TOOLCHAIN_PREFIX)ld

ifeq ($(TOOLCHAIN),llvm)
    CC := clang
    LD := ld.lld
endif

CFLAGS := -g -O2 -pipe
CPPFLAGS :=

ifeq ($(SYSCALL_LOG),1)
override CPPFLAGS += -DSYSCALL_LOG=1
endif

# Diagnostics knobs.
#
# The hang/deadlock detector in kernel/src/lock/lockdiag.c is armed by default:
# its cost is a couple of stores per timer tick and a few relaxed atomics per
# futex operation, and the point of it is that a normal build left running an
# XFCE session produces a report instead of a silent serial line.
#
#   make LOCKDIAG=0           compile the detector out entirely
#   make LOCKDIAG_OWNER=1     also record every spinlock each CPU holds, and
#                             where it took it from.  This adds a store plus a
#                             shadow-stack push to every spinlock in the kernel
#                             (slab allocator included), so it is a rebuild for
#                             chasing a specific hang rather than a default.
#   make LOCKDIAG_HUNG_MS=2000  notice a stopped core sooner
#   make LOCKDIAG_STUCK_MS=30000  enumerate futex waiters periodically.  Off by
#                             default: idle worker pools (tumblerd's, xfdesktop's)
#                             park in FUTEX_WAIT forever on a healthy desktop, and
#                             from the kernel that is indistinguishable from a
#                             lost wakeup, so the periodic lines were noise.  Long
#                             parks are still listed inside a real hang report.
#   make LOCKDIAG_SELFTEST=1  render one full report at boot on a healthy machine
ifeq ($(LOCKDIAG),0)
override CPPFLAGS += -DLOCKDIAG=0
endif

ifeq ($(LOCKDIAG_OWNER),1)
override CPPFLAGS += -DLOCKDIAG_OWNER=1
endif

ifneq ($(LOCKDIAG_HUNG_MS),)
override CPPFLAGS += -DLOCKDIAG_HUNG_CPU_MS=$(LOCKDIAG_HUNG_MS)ULL
endif

ifneq ($(LOCKDIAG_STUCK_MS),)
override CPPFLAGS += -DLOCKDIAG_STUCK_TASK_MS=$(LOCKDIAG_STUCK_MS)ULL
endif

# Renders one full hang report at boot on a healthy machine, so the report code
# is exercised somewhere other than during a real hang.
ifeq ($(LOCKDIAG_SELFTEST),1)
override CPPFLAGS += -DLOCKDIAG_SELFTEST=1
endif

# NVMe controller self-tests at boot (disable/enable/Identify cycles).  Used by
# scripts/nvme-stress.sh; a normal build compiles them out.
#
#   make NVME_SELFTEST=1                    50 cycles (default)
#   make NVME_SELFTEST=1 NVME_SELFTEST_CYCLES=1000
#   make NVME_SELFTEST=1 NVME_SELFTEST_IDENTIFY_OPS=10000
ifeq ($(NVME_SELFTEST),1)
override CPPFLAGS += -DNVME_SELFTEST=1
ifneq ($(NVME_SELFTEST_CYCLES),)
override CPPFLAGS += -DNVME_SELFTEST_CYCLES=$(NVME_SELFTEST_CYCLES)
endif
ifneq ($(NVME_SELFTEST_IDENTIFY_OPS),)
override CPPFLAGS += -DNVME_SELFTEST_IDENTIFY_OPS=$(NVME_SELFTEST_IDENTIFY_OPS)
endif
ifeq ($(NVME_SELFTEST_FAULT_TIMEOUT),1)
override CPPFLAGS += -DNVME_SELFTEST_FAULT_TIMEOUT=1
ifneq ($(NVME_SELFTEST_FAULT_INDEX),)
override CPPFLAGS += -DNVME_SELFTEST_FAULT_INDEX=$(NVME_SELFTEST_FAULT_INDEX)u
endif
endif
endif

# NVMe completion interrupt knobs (Phase 4).  The driver falls back through
# MSI-X -> MSI -> INTx -> polled; these force a lower mode for the stress
# matrix.  NVME_FAULT_DROP_IRQ consumes completions in the handler but delays
# the waiter wakeup, exercising the scheduler-tick fallback.
ifeq ($(NVME_NOIRQ),1)
override CPPFLAGS += -DNVME_NOIRQ=1
endif
ifeq ($(NVME_DISABLE_MSIX),1)
override CPPFLAGS += -DNVME_DISABLE_MSIX=1
endif
ifeq ($(NVME_DISABLE_MSI),1)
override CPPFLAGS += -DNVME_DISABLE_MSI=1
endif
ifeq ($(NVME_FAULT_DROP_IRQ),1)
override CPPFLAGS += -DNVME_FAULT_DROP_IRQ=1
endif

# NVMe Phase 5 knobs.
#   NVME_SELFTEST_DATA=1     allow the destructive 4Kn/multi-NS/recovery
#                            self-tests (scratch media only)
#   NVME_FUA=1               issue every block write with FUA
#   NVME_RESET_RECOVERY=1    reset + retry an I/O command after a timeout
ifeq ($(NVME_SELFTEST_DATA),1)
override CPPFLAGS += -DNVME_SELFTEST_DATA=1
endif
ifeq ($(NVME_FUA),1)
override CPPFLAGS += -DNVME_FUA=1
endif
ifeq ($(NVME_RESET_RECOVERY),1)
override CPPFLAGS += -DNVME_RESET_RECOVERY=1
endif

ifeq ($(ARCH),x86_64)
    NASMFLAGS := -g
endif

LDFLAGS :=


override CC_IS_CLANG := $(shell ! $(CC) --version 2>/dev/null | grep -q '^Target: '; echo $$?)

override CFLAGS += \
    -Wall \
    -Wextra \
    -std=gnu11 \
    -nostdinc \
    -ffreestanding \
    -fno-stack-protector \
    -fno-stack-check \
    -fno-lto \
    -fno-PIC \
    -ffunction-sections \
    -fdata-sections

override CPPFLAGS := \
    -I src \
    -I src/include \
    -I limine-protocol/include \
    -isystem freestnd-c-hdrs/include \
    $(CPPFLAGS) \
    -MMD \
    -MP

ifeq ($(ARCH),x86_64)
    override NASMFLAGS := \
        $(patsubst -g,-g -F dwarf,$(NASMFLAGS)) \
        -Wall
endif

ifeq ($(ARCH),x86_64)
    ifeq ($(CC_IS_CLANG),1)
        override CC += \
            -target x86_64-unknown-none-elf
    endif
    override CFLAGS += \
        -m64 \
        -march=x86-64 \
        -mabi=sysv \
        -mno-80387 \
        -mno-mmx \
        -mno-sse \
        -mno-sse2 \
        -mno-red-zone \
        -mcmodel=kernel
    override LDFLAGS += \
        -m elf_x86_64
    override NASMFLAGS := \
        -f elf64 \
        $(NASMFLAGS)
endif

override LDFLAGS += \
    -nostdlib \
    -static \
    -z max-page-size=0x1000 \
    --gc-sections \
    -T linker-scripts/$(ARCH).lds

override SRCFILES := $(shell find -L src cc-runtime/src -type f -not -path 'src/arch/*' 2>/dev/null | LC_ALL=C sort)
override SRCFILES += $(shell find -L src/arch/$(ARCH) -type f 2>/dev/null | LC_ALL=C sort)

override CFILES := $(filter-out src/syscalls/vdso.c,$(filter %.c,$(SRCFILES)))
override ASFILES := $(filter %.S,$(SRCFILES))
ifeq ($(ARCH),x86_64)
override NASMFILES := $(filter %.asm,$(SRCFILES))
endif
override OBJ := $(addprefix obj-$(ARCH)/,$(CFILES:.c=.c.o) $(ASFILES:.S=.S.o))
ifeq ($(ARCH),x86_64)
override OBJ += $(addprefix obj-$(ARCH)/,$(NASMFILES:.asm=.asm.o))
endif
override HEADER_DEPS := $(addprefix obj-$(ARCH)/,$(CFILES:.c=.c.d) $(ASFILES:.S=.S.d))

.PHONY: all
all: bin-$(ARCH)/$(OUTPUT)

-include $(HEADER_DEPS)

src/syscalls/vdso.so: src/syscalls/vdso.c src/syscalls/vdso.lds src/syscalls/vdso.version
	gcc -shared -nostdlib -fPIC -fno-stack-protector -O2 -Wl,-T,src/syscalls/vdso.lds -Wl,-soname=linux-vdso.so.1 -Wl,--version-script=src/syscalls/vdso.version -Wl,--hash-style=both -Wl,-Bsymbolic -Wl,-z,max-page-size=4096 -o $@ $<

obj-$(ARCH)/src/syscalls/vdso_blob.asm.o: src/syscalls/vdso_blob.asm src/syscalls/vdso.so GNUmakefile
	mkdir -p "$(dir $@)"
	nasm -I src/syscalls/ $(NASMFLAGS) $< -o $@

bin-$(ARCH)/$(OUTPUT): GNUmakefile linker-scripts/$(ARCH).lds $(OBJ)
	mkdir -p "$(dir $@)"
	$(LD) $(LDFLAGS) $(OBJ) -o $@

obj-$(ARCH)/%.c.o: %.c GNUmakefile
	mkdir -p "$(dir $@)"
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

obj-$(ARCH)/%.S.o: %.S GNUmakefile
	mkdir -p "$(dir $@)"
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

ifeq ($(ARCH),x86_64)
obj-$(ARCH)/%.asm.o: %.asm GNUmakefile
	mkdir -p "$(dir $@)"
	nasm $(NASMFLAGS) $< -o $@
endif

.PHONY: clean
clean:
	rm -rf bin-$(ARCH) obj-$(ARCH)

.PHONY: distclean
distclean:
	rm -rf bin-* obj-* .cache compile_commands.json
