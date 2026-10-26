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

# Release identity is supplied by the repository's top-level make invocation.
BUILD_NAME ?= Ascension
BUILD_VERSION ?= 3.0.0
BUILD_TAG ?= dev
override CPPFLAGS += -DAVORY_BUILD_NAME='"$(BUILD_NAME)"' \
	-DAVORY_BUILD_VERSION='"$(BUILD_VERSION)"' \
	-DAVORY_BUILD_TAG='"$(BUILD_TAG)"'

# Syscall tracing.
#
#   make SYSCALL_LOG=1         every syscall of every process (very noisy)
#   make SYSCALL_LOG=browser   only the badwolf/WebKit/bwrap process tree
#   make SYSCALL_LOG=sddm      only the sddm/Xorg/greeter process tree
#
# The filtered modes mark a task when it execs a matching path and inherit
# the mark through fork/clone; they also log syscall errors and every open()
# path (see kernel/src/syscalls/syscall.c).
ifeq ($(SYSCALL_LOG),1)
override CPPFLAGS += -DSYSCALL_LOG=1
else ifeq ($(SYSCALL_LOG),browser)
override CPPFLAGS += -DSYSCALL_LOG=2
else ifeq ($(SYSCALL_LOG),sddm)
override CPPFLAGS += -DSYSCALL_LOG=3
endif

# /proc/klog ring buffer size, in KiB (default 256).  Browser syscall traces
# produce megabytes, so a tracing boot usually wants a larger ring:
#   make SYSCALL_LOG=browser KLOG_RING_KB=8192
ifneq ($(KLOG_RING_KB),)
override CPPFLAGS += -DKLOG_RING_KB=$(KLOG_RING_KB)
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
# scripts/nvme/nvme-stress.sh; a normal build compiles them out.
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

# USB HID mouse parser self-test.  Parses built-in report descriptors
# (boot / gaming / touchpad layouts) and decodes sample reports through the
# same code path the interrupt handlers use.  A normal build compiles it out.
#
#   make USB_MOUSE_SELFTEST=1
ifeq ($(USB_MOUSE_SELFTEST),1)
override CPPFLAGS += -DUSB_MOUSE_SELFTEST=1
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
#   NVME_BATCH_IO=0          disable the pipelined block-I/O path
#   NVME_BATCH_IO=1          advertise block read_batch/write_batch ops so a
#                            single submitter can pipeline up to
#                            NVME_BATCH_WAVE_MAX commands per wait
NVME_BATCH_IO ?= 1
ifeq ($(NVME_SELFTEST_DATA),1)
override CPPFLAGS += -DNVME_SELFTEST_DATA=1
endif
ifeq ($(NVME_FUA),1)
override CPPFLAGS += -DNVME_FUA=1
endif
ifeq ($(NVME_RESET_RECOVERY),1)
override CPPFLAGS += -DNVME_RESET_RECOVERY=1
endif
ifeq ($(NVME_BATCH_IO),1)
override CPPFLAGS += -DNVME_BATCH_IO=1
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
    -I linuxkpi/include \
    -I limine-protocol/include \
    -isystem freestnd-c-hdrs/include \
    $(CPPFLAGS) \
    -MMD \
    -MP

# ── Imported Linux sources (Phase 0) ─────────────────────────────────────────
#
# kernel/linux/ holds a pinned upstream Linux subset vendored by
# scripts/linux-import.sh.  The script also writes kernel/linux/Makefile.files,
# which lists the upstream .c files compiled into the kernel; both the tree and
# the fragment are absent until the script has run, and the kernel builds fine
# without them (LINUX_OBJ stays empty).
#
# Imported code gets its own include path order: the real Linux headers win,
# and the legacy stub headers in src/include are deliberately NOT on the path,
# so a missing import fails loudly instead of silently using a stub.
LINUX_OBJ :=
LINUX_CPPFLAGS :=
LINUX_CFLAGS :=
-include linux/Makefile.files
ifneq ($(linux-obj-y),)
    override LINUX_CPPFLAGS := \
        -I linuxkpi/include \
        -I ../build/firmware \
        -I ../build/nouveau/firmware \
        -I ../build/nouveau-618/firmware \
        -I linux/arch/x86/include \
        -I linux/arch/x86/include/generated \
        -I linux/include \
        -I linux/arch/x86/include/uapi \
        -I linux/arch/x86/include/generated/uapi \
        -I linux/include/uapi \
        -I linux/include/generated/uapi \
        -I linux/lib \
        -I limine-protocol/include \
        -isystem freestnd-c-hdrs/include \
        -D__KERNEL__ \
        -DKBUILD_MODNAME='"avoryos"' \
        -DKBUILD_BASENAME='"avoryos"' \
        -include linux/include/linux/compiler-version.h \
        -include linux/include/linux/compiler_types.h \
        -include linux/include/generated/autoconf.h \
        -include linux/include/linux/kconfig.h \
        $(filter -D%,$(CPPFLAGS)) \
        -MMD \
        -MP
    override LINUX_CFLAGS := \
        -fno-strict-aliasing \
        -fno-common \
        -fshort-wchar \
        -Wno-unused-parameter \
        -Wno-unused-function \
        -Wno-unused-but-set-variable \
        -Wno-sign-compare \
        -Wno-missing-field-initializers
    override CPPFLAGS += -DLINUXKPI_LINUX_IMPORTED=1

    # Phase 6 (amdgpu): generated object list + include paths + per-file flags
    # from the pinned Kbuild fragments (scripts/linux/kbuild-subset.py).
    # KPI_AMDGPU defaults to 1 now that 6a links cleanly; the boot-safety gate
    # (kpi_amdgpu, default 1 since C3 in linuxkpi/src/pci.c) lets amdgpu probe
    # the passed GPU, with `kpi_amdgpu=0` as the C2 revert.  `make KPI_AMDGPU=0`
    # returns to the Phase 5 baseline object set.
    # Makefile.kbuild must be included before LINUX_OBJ is computed (it
    # appends to linux-obj-y) but after LINUX_CPPFLAGS/LINUX_CFLAGS (the
    # generated per-target rules append to them at parse time).
    KPI_AMDGPU ?= 1
    KPI_NOUVEAU ?= 1
    # Keep parameter namespaces distinct even in a single static kernel.
    obj-$(ARCH)/linux/drivers/gpu/drm/amd/%.c.o: override LINUX_CPPFLAGS += -UKBUILD_MODNAME -DKBUILD_MODNAME='"amdgpu"'
    obj-$(ARCH)/linux/drivers/gpu/drm/nouveau/%.c.o: override LINUX_CPPFLAGS += -UKBUILD_MODNAME -DKBUILD_MODNAME='"nouveau"'
    obj-$(ARCH)/linux/drivers/gpu/drm/drm_print.c.o: override LINUX_CPPFLAGS += -UKBUILD_MODNAME -DKBUILD_MODNAME='"drm"'
    -include linux/Makefile.kbuild
    ifeq ($(KPI_NOUVEAU),1)
    obj-$(ARCH)/linux/drivers/gpu/drm/nouveau/nouveau_sched.c.o: override LINUX_CPPFLAGS += -Ddma_fence_wait_timeout=linuxkpi_nouveau_bind_wait_timeout
    obj-$(ARCH)/linux/drivers/gpu/drm/nouveau/nvkm/engine/device/pci.c.o: override LINUX_CPPFLAGS += -Dnvkm_device_pci_new=linuxkpi_original_nvkm_device_pci_new
    obj-$(ARCH)/linuxkpi/src/nouveau_probe_unwind.c.o: override LINUX_CPPFLAGS += -DAVORYOS_NOUVEAU_COMPAT -Ilinux/drivers/gpu/drm/nouveau/include/nvkm -Ilinux/drivers/gpu/drm/nouveau/include
    obj-$(ARCH)/src/tests/linuxkpi/linux/test_nouveau_firmware_failures.c.o: override LINUX_CPPFLAGS += -DAVORYOS_NOUVEAU_COMPAT -Ilinux/drivers/gpu/drm/nouveau/include/nvkm -Ilinux/drivers/gpu/drm/nouveau/include -Ilinux/drivers/gpu/drm/nouveau/nvkm/subdev/gsp
    endif

    override LINUX_OBJ := \
        $(addprefix obj-$(ARCH)/linux/,$(linux-obj-y:.c=.c.o))

    # AvoryOS-authored LinuxKPI implementations and Linux-API boot tests.
    # Both are compiled only when the Linux tree exists, because they include
    # the real Linux headers.  See the pattern rules below.
    KPI_SRCFILES := $(shell find -L linuxkpi/src -type f -name '*.c' 2>/dev/null | LC_ALL=C sort)
    KPI_OBJ := $(addprefix obj-$(ARCH)/,$(KPI_SRCFILES:.c=.c.o))
    # UUIDs currently serve DisplayPort identifiers only. Do not export a
    # weak implementation of Linux cryptographic get_random_bytes().
    obj-$(ARCH)/linux/lib/uuid.c.o: override LINUX_CPPFLAGS += -Dget_random_bytes=linuxkpi_guid_bytes
    obj-$(ARCH)/linux/drivers/gpu/drm/drm_syncobj.c.o: override LINUX_CPPFLAGS += -Ddrm_syncobj_timeline_wait_ioctl=linuxkpi_original_drm_syncobj_timeline_wait_ioctl
    # Empty-manager eviction in the pinned import reads an undefined result.
    # LinuxKPI supplies the corrected entry point without editing the import.
    obj-$(ARCH)/linux/drivers/gpu/drm/ttm/ttm_resource.c.o: override LINUX_CPPFLAGS += -Dttm_resource_manager_evict_all=linuxkpi_original_ttm_resource_manager_evict_all
    # AvoryOS owns the AMD userspace allocation entry point. Keep the
    # imported implementation pristine and compile it under its bridge name.
    ifeq ($(KPI_AMDGPU),1)
    obj-$(ARCH)/linux/drivers/gpu/drm/amd/amdgpu/amdgpu_gem.c.o: override LINUX_CPPFLAGS += -Damdgpu_gem_create_ioctl=linuxkpi_amdgpu_gem_create_ioctl
    # Release kernel doorbells while the TTM managers still own their BO.
    obj-$(ARCH)/linux/drivers/gpu/drm/amd/amdgpu/amdgpu_ttm.c.o: override LINUX_CPPFLAGS += -Damdgpu_ttm_fini=linuxkpi_original_amdgpu_ttm_fini
    obj-$(ARCH)/linux/drivers/gpu/drm/amd/amdgpu/amdgpu_ib.c.o: override LINUX_CPPFLAGS += -Damdgpu_ib_schedule=linuxkpi_original_amdgpu_ib_schedule
    obj-$(ARCH)/linux/drivers/gpu/drm/amd/amdgpu/amdgpu_device.c.o: override LINUX_CPPFLAGS += -include linuxkpi/include/linuxkpi/amdgpu_init_reset.h
    obj-$(ARCH)/linux/drivers/gpu/drm/amd/amdgpu/amdgpu_gem.c.o: override LINUX_CPPFLAGS += -Damdgpu_gem_va_ioctl=linuxkpi_original_amdgpu_gem_va_ioctl
    obj-$(ARCH)/linux/drivers/gpu/drm/amd/amdgpu/gmc_v10_0.c.o obj-$(ARCH)/linux/drivers/gpu/drm/amd/amdgpu/amdgpu_job.c.o: override LINUX_CPPFLAGS += -Damdgpu_vm_get_task_info=linuxkpi_amdgpu_vm_fault_info -Damdgpu_vm_get_task_info_pasid=linuxkpi_amdgpu_vm_fault_info_pasid
    obj-$(ARCH)/linux/drivers/gpu/drm/amd/amdgpu/amdgpu_vm.c.o: override LINUX_CPPFLAGS += -Damdgpu_vm_bo_update=linuxkpi_original_amdgpu_vm_bo_update -Dtrace_amdgpu_vm_bo_update=trace_linuxkpi_original_amdgpu_vm_bo_update
    obj-$(ARCH)/linux/drivers/gpu/drm/amd/amdgpu/amdgpu_fence.c.o: override LINUX_CPPFLAGS += -Damdgpu_fence_emit=linuxkpi_original_amdgpu_fence_emit
    obj-$(ARCH)/linux/drivers/gpu/drm/amd/amdgpu/amdgpu_psp.c.o: override LINUX_CPPFLAGS += -Dpsp_wait_for=linuxkpi_original_psp_wait_for
    obj-$(ARCH)/linux/drivers/gpu/drm/amd/amdgpu/amdgpu_device.c.o: override LINUX_CPPFLAGS += -Damdgpu_device_ip_set_powergating_state=linuxkpi_original_amdgpu_device_ip_set_powergating_state
    obj-$(ARCH)/linux/drivers/gpu/drm/amd/amdgpu/amdgpu_vcn.c.o: override LINUX_CPPFLAGS += -Damdgpu_vcn_dec_ring_test_ring=linuxkpi_original_amdgpu_vcn_dec_ring_test_ring
    obj-$(ARCH)/linux/drivers/gpu/drm/amd/amdgpu/amdgpu_psp.c.o: override LINUX_CPPFLAGS += -include linuxkpi/include/linuxkpi/psp_ring_trace.h
    obj-$(ARCH)/linux/drivers/dma-buf/dma-fence.c.o: override LINUX_CPPFLAGS += -Ddma_fence_signal=linuxkpi_original_dma_fence_signal
    obj-$(ARCH)/linux/drivers/dma-buf/dma-resv.c.o: override LINUX_CPPFLAGS += -Ddma_resv_add_fence=linuxkpi_original_dma_resv_add_fence
    obj-$(ARCH)/linuxkpi/src/amdgpu_memory.c.o obj-$(ARCH)/linuxkpi/src/amdgpu_trace.c.o obj-$(ARCH)/linuxkpi/src/amdgpu_teardown.c.o obj-$(ARCH)/linuxkpi/src/amdgpu_init_reset.c.o obj-$(ARCH)/linuxkpi/src/psp_submit_trace.c.o obj-$(ARCH)/linuxkpi/src/psp_completion_trace.c.o obj-$(ARCH)/linuxkpi/src/psp_reinit.c.o obj-$(ARCH)/linuxkpi/src/amdgpu_ib_visibility.c.o: override LINUX_CPPFLAGS += \
        -DAVORYOS_AMDGPU_MEMORY_POLICY \
        -Ilinux/drivers/gpu/drm/amd/amdgpu \
        -Ilinux/drivers/gpu/drm/amd/include \
        -Ilinux/drivers/gpu/drm/amd/pm/inc \
        -Ilinux/drivers/gpu/drm/amd/display \
        -Ilinux/drivers/gpu/drm/amd/display/include \
        -Ilinux/drivers/gpu/drm/amd/display/modules/inc \
        -Ilinux/drivers/gpu/drm/amd/display/dc \
        -Ilinux/drivers/gpu/drm/amd/display/amdgpu_dm
    endif
    LINUX_TEST_SRCFILES := $(shell find -L src/tests/linuxkpi/linux -type f -name '*.c' 2>/dev/null | LC_ALL=C sort)
    LINUX_TEST_OBJ := $(addprefix obj-$(ARCH)/,$(LINUX_TEST_SRCFILES:.c=.c.o))
endif

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
# Linux-API tests are compiled with Linux headers by a dedicated rule below;
# keep them out of the native compile when the tree is absent.
override CFILES := $(filter-out src/tests/linuxkpi/linux/%,$(CFILES))
override ASFILES := $(filter %.S,$(SRCFILES))
ifeq ($(ARCH),x86_64)
override NASMFILES := $(filter %.asm,$(SRCFILES))
endif
override OBJ := $(addprefix obj-$(ARCH)/,$(CFILES:.c=.c.o) $(ASFILES:.S=.S.o))
ifeq ($(ARCH),x86_64)
override OBJ += $(addprefix obj-$(ARCH)/,$(NASMFILES:.asm=.asm.o))
endif
override OBJ += $(KPI_OBJ) $(LINUX_TEST_OBJ)
override HEADER_DEPS := $(addprefix obj-$(ARCH)/,$(CFILES:.c=.c.d) $(ASFILES:.S=.S.d))
override HEADER_DEPS += $(LINUX_OBJ:.o=.d) $(KPI_OBJ:.o=.d) $(LINUX_TEST_OBJ:.o=.d)

.PHONY: all
all: bin-$(ARCH)/$(OUTPUT)

ifneq ($(linux-obj-y),)
# Adding an overlay must invalidate objects that previously included the
# imported header and inlined its unavailable-eventfd stubs. Keep this after
# all so an individual object does not become the default build target.
obj-$(ARCH)/linux/drivers/gpu/drm/drm_syncobj.c.o: linuxkpi/include/linux/eventfd.h
endif

# Imported headers/layouts and AMD-specific bridge compilation differ between
# source profiles. Switching a pin or gate must also rebuild native/KPI/test
# objects, even when the alternate source's timestamps are older.  Keep build
# identity separate: it is embedded only by kernel.c and sys_process.c, so a
# new source fingerprint need not invalidate every kernel object.
LINUX_PROFILE_STAMP := obj-$(ARCH)/.linux-profile
LINUX_IMPORT_ID := $(shell sed -n '/^tag=/p;/^commit=/p' linux/.pin 2>/dev/null)
.PHONY: linux-profile-force
$(LINUX_PROFILE_STAMP): linux-profile-force
	@mkdir -p "$(dir $@)"
	@printf '%s\n' '$(LINUX_IMPORT_ID) KPI_AMDGPU=$(KPI_AMDGPU) KPI_NOUVEAU=$(KPI_NOUVEAU) LINUX_IMPORTED=$(if $(linux-obj-y),1,0)' > $@.tmp
	@if ! cmp -s $@.tmp $@; then mv $@.tmp $@; else rm $@.tmp; fi
$(OBJ) $(LINUX_OBJ): $(LINUX_PROFILE_STAMP)

# Only these translation units embed the public build identity macros. Keep
# their dependencies narrow so source edits rebuild changed objects and their
# header dependents without recompiling the whole kernel.
BUILD_ID_STAMP := obj-$(ARCH)/.build-identity
.PHONY: build-id-force
$(BUILD_ID_STAMP): build-id-force
	@mkdir -p "$(dir $@)"
	@printf '%s\n' 'BUILD_NAME=$(BUILD_NAME) BUILD_VERSION=$(BUILD_VERSION) BUILD_TAG=$(BUILD_TAG)' > $@.tmp
	@if ! cmp -s $@.tmp $@; then mv $@.tmp $@; else rm -f $@.tmp; fi
obj-$(ARCH)/src/kernel.c.o obj-$(ARCH)/src/syscalls/sys_process.c.o: $(BUILD_ID_STAMP)

# NVME_BATCH_IO changes only the NVMe driver's registered block operations.
# Track it separately so enabling or disabling the path rebuilds that object
# without invalidating the rest of the kernel.
NVME_BATCH_STAMP := obj-$(ARCH)/.nvme-batch-io
.PHONY: nvme-batch-force
$(NVME_BATCH_STAMP): nvme-batch-force
	@mkdir -p "$(dir $@)"
	@printf '%s\n' 'NVME_BATCH_IO=$(NVME_BATCH_IO)' > $@.tmp
	@if ! cmp -s $@.tmp $@; then mv $@.tmp $@; else rm -f $@.tmp; fi
obj-$(ARCH)/src/drivers/storage/nvme.c.o: $(NVME_BATCH_STAMP)

-include $(HEADER_DEPS)

src/syscalls/vdso.so: src/syscalls/vdso.c src/syscalls/vdso.lds src/syscalls/vdso.version
	gcc -shared -nostdlib -fPIC -fno-stack-protector -O2 -Wl,-T,src/syscalls/vdso.lds -Wl,-soname=linux-vdso.so.1 -Wl,--version-script=src/syscalls/vdso.version -Wl,--hash-style=both -Wl,-Bsymbolic -Wl,-z,max-page-size=4096 -o $@ $<

obj-$(ARCH)/src/syscalls/vdso_blob.asm.o: src/syscalls/vdso_blob.asm src/syscalls/vdso.so GNUmakefile
	mkdir -p "$(dir $@)"
	nasm -I src/syscalls/ $(NASMFLAGS) $< -o $@

bin-$(ARCH)/$(OUTPUT): GNUmakefile linker-scripts/$(ARCH).lds $(OBJ) $(LINUX_OBJ)
	mkdir -p "$(dir $@)"
	$(LD) $(LDFLAGS) $(OBJ) $(LINUX_OBJ) -o $@

obj-$(ARCH)/%.c.o: %.c GNUmakefile
	mkdir -p "$(dir $@)"
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

# Imported Linux sources.  This pattern is more specific than the generic one
# above, so make prefers it for obj-$(ARCH)/linux/... targets.
obj-$(ARCH)/linux/%.c.o: linux/%.c GNUmakefile
	mkdir -p "$(dir $@)"
	$(CC) $(filter-out $(LINUX_REMOVE_CFLAGS),$(CFLAGS)) $(LINUX_CPPFLAGS) $(filter-out $(LINUX_REMOVE_CFLAGS),$(LINUX_CFLAGS)) -c $< -o $@

ifneq ($(linux-obj-y),)
# AvoryOS LinuxKPI implementations.  They speak the Linux API (real Linux
# headers with linuxkpi/include overlaid first) and may also include native
# kernel headers via -I src when they need AvoryOS primitives.
obj-$(ARCH)/linuxkpi/src/%.c.o: linuxkpi/src/%.c GNUmakefile
	mkdir -p "$(dir $@)"
	$(CC) $(CFLAGS) $(LINUX_CPPFLAGS) $(LINUX_CFLAGS) -I src -I src/include -c $< -o $@

# Boot tests that are written against the Linux API directly.
obj-$(ARCH)/src/tests/linuxkpi/linux/%.c.o: src/tests/linuxkpi/linux/%.c GNUmakefile
	mkdir -p "$(dir $@)"
	$(CC) $(CFLAGS) $(LINUX_CPPFLAGS) $(LINUX_CFLAGS) -I src -I src/include -c $< -o $@
endif

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
