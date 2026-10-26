.SUFFIXES:

.DEFAULT_GOAL := all

ARCH := x86_64
# QEMU audio backend latency (microseconds).  The guest HDA driver targets tens
# of milliseconds end to end, so a 500 ms host backend would swamp it.  Raise
# AUDIO_LATENCY/AUDIO_BUFLEN if the host audio crackles on a loaded machine.
AUDIO_LATENCY ?= 50000
AUDIO_BUFLEN ?= 100000
# An absolute tablet keeps GTK from warping the host pointer on DRM cursor
# moves or hiding it under a relative-mouse grab.
# Grab only the keyboard on hover so desktop shortcuts reach the guest.
QEMU_MEM ?= -m 4G
QEMUFLAGS := $(QEMU_MEM) \
	-vga none \
	-device virtio-vga,xres=1280,yres=800 \
	-display gtk,zoom-to-fit=off,grab-on-hover=on

override IMAGE_NAME := avoryos-$(ARCH)

# Public release identity embedded in the kernel. BUILD_TAG is generated and
# persisted for each distinct kernel source state by scripts/kernel-build-tag.py.
BUILD_NAME ?= Ascension
BUILD_VERSION ?= 3.0.0

# Boot the existing writable disk without traversing its rebuild prerequisites.
# A missing disk is built normally; use `make disk.img` to refresh it explicitly.
RUN_DISK_DEP := $(if $(wildcard disk.img),,disk.img)

QUAKE2_BUNDLE_FILES := \
	userland/quake2/quake2 \
	userland/quake2/ref_soft.so \
	userland/quake2/baseq2/game.so \
	userland/quake2/baseq2/pak0.pak \
	userland/quake2/baseq2/autoexec.cfg

QUAKE2_STAMP := build/quake2/.built

# The Quake II script produces the engine, renderer, game library, config, and
# demo data as one bundle. A stamp file tracks whether the build is current so
# editing the build script or evdev header doesn't re-trigger a full rebuild
# unless the outputs are actually missing.
$(QUAKE2_BUNDLE_FILES): $(QUAKE2_STAMP)

$(QUAKE2_STAMP): $(ARCH_STAMP) $(GLIBC_STAMP) $(GLIBC_TOOLCHAIN_STAMP) scripts/build-quake2.sh \
		scripts/quake2-sdl2-config.in
	./scripts/build-quake2.sh && \
		mkdir -p $(dir $(QUAKE2_STAMP)) && \
		touch $(QUAKE2_STAMP)

HOST_CC := cc
HOST_CFLAGS := -g -O2 -pipe
HOST_CPPFLAGS :=
HOST_LDFLAGS :=
HOST_LIBS :=

# Prebuilt GCC cross-compiler; libc headers and libraries come from Axon's
# target rootfs (currently glibc 2.44).
GLIBC_TOOLCHAIN_BIN := $(CURDIR)/toolchain/x86_64-linux-glibc/bin
ARCH_SYSROOT := $(CURDIR)/build/arch/rootfs
ARCH_STAMP := $(CURDIR)/build/arch/.built
GLIBC_SYSROOT := $(ARCH_SYSROOT)
GLIBC_STAMP := $(ARCH_STAMP)
GLIBC_TOOLCHAIN_STAMP := $(CURDIR)/build/.glibc-toolchain-ready
GLIBC_CC := $(GLIBC_TOOLCHAIN_BIN)/x86_64-linux-gcc
GLIBC_CXX := $(GLIBC_TOOLCHAIN_BIN)/x86_64-linux-g++
GLIBC_USER_CFLAGS := -O2 -Wall -Wextra -fno-stack-protector \
	--sysroot=$(GLIBC_SYSROOT) -Wl,-T,$(CURDIR)/config/userland-link.ld
GLIBC_USER_CXXFLAGS := -O2 -Wall -Wextra -fno-stack-protector \
	--sysroot=$(GLIBC_SYSROOT) -Wl,-T,$(CURDIR)/config/userland-link.ld

AXON_PROFILE_FILES := $(wildcard config/xorg/*) config/vlc/vlc.sh \
	config/wireplumber/20-avoryos-hda.conf userland/avory-account \
	userland/test_accounts.sh

userland/avory-login.elf: userland/avory-login.c userland/GNUmakefile config/userland-link.ld $(GLIBC_STAMP) $(GLIBC_TOOLCHAIN_STAMP)
	$(MAKE) -C userland -f GNUmakefile avory-login.elf


$(ARCH_STAMP): scripts/setup-rootfs.sh scripts/axon-common.sh scripts/configure-axon-mirrors.py \
		scripts/rootfs-packages.txt $(AXON_PROFILE_FILES) \
		userland/avory-login.c $(GLIBC_TOOLCHAIN_STAMP)
	@mkdir -p $(dir $(ARCH_STAMP))
	@bash scripts/setup-rootfs.sh
	@touch $(ARCH_STAMP)

.PHONY: setup-rootfs
setup-rootfs: $(ARCH_STAMP)


$(GLIBC_TOOLCHAIN_STAMP): scripts/glibc-toolchain.sh
	@echo "[*] Setting up the prebuilt GCC cross-compiler..."
	chmod +x scripts/glibc-toolchain.sh && ./scripts/glibc-toolchain.sh
	@test -x $(GLIBC_CC)
	@mkdir -p $(dir $@)
	@touch $@

.PHONY: glibc-toolchain
glibc-toolchain: $(GLIBC_TOOLCHAIN_STAMP) $(GLIBC_STAMP)

.PHONY: all
all: $(IMAGE_NAME).iso disk.img edk2-ovmf

# `make run` boots the NVMe desktop with KVM; `make run-tcg` is the same
# configuration without KVM.  The arch-specific run-x86_64 target remains the
# TCG implementation behind run-tcg.
RUN_SMP ?= 8
# NIC selection for launchers that explicitly attach networking:
#   make run RUN_NIC=e1000    # 82540EM networking
#   make run RUN_NIC=both     # both drivers; RTL8139 is default
#   make run RUN_NIC=both KERNEL_CMDLINE='net_default=eth1' # use e1000
#   make run RUN_NIC=none     # no emulated NIC
RUN_NIC ?= rtl8139
ifneq ($(words $(RUN_NIC)),1)
$(error RUN_NIC must be rtl8139, e1000, both, or none)
endif
ifeq ($(filter $(RUN_NIC),rtl8139 e1000 both none),)
$(error RUN_NIC must be rtl8139, e1000, both, or none)
endif
RUN_NIC_ADDR ?=
NET_COMMA := ,
RUN_NET_FLAGS = -nic none -device $(if $(filter both,$(RUN_NIC)),rtl8139,$(RUN_NIC)),netdev=net0$(if $(RUN_NIC_ADDR),$(NET_COMMA)addr=$(RUN_NIC_ADDR)) -netdev user,id=net0
ifeq ($(RUN_NIC),both)
RUN_NET_FLAGS += -device e1000,netdev=net1 -netdev user,id=net1
endif
ifeq ($(RUN_NIC),none)
RUN_NET_FLAGS = -nic none
endif
# Match run-nouveau's xHCI keyboard/relative-mouse setup. Xorg draws its
# software cursor and the frontend keeps the host cursor hidden. An absolute tablet
# remains available through RUN_POINTER=usb-tablet.
RUN_POINTER ?= usb-mouse
# Virgl requires QEMU's OpenGL device and an OpenGL display backend.
# RUN_VIRGL=0 keeps a 2D VM available on hosts without virglrenderer/EGL.
RUN_VIRGL ?= 1
# Guard invalid vertex-buffer draws before NVIDIA's host OpenGL driver sees
# them. Built locally; system QEMU and graphics packages are not replaced.
RUN_VIRGL_HOST_FIX ?= 1
VIRGL_HOST_LIB := build/virgl-host/lib/libvirglrenderer.so.1
RUN_VIRGL_FIXED = $(if $(filter 1,$(RUN_VIRGL)),$(filter 1,$(RUN_VIRGL_HOST_FIX)))
RUN_VIRGL_ENV = $(if $(RUN_VIRGL_FIXED),env LD_LIBRARY_PATH="$(abspath build/virgl-host/lib):$${LD_LIBRARY_PATH:-}")
$(VIRGL_HOST_LIB): scripts/build-virgl-host.py scripts/virgl/missing-vertex-buffer.patch scripts/virgl/missing-vertex-buffer-v1.patch scripts/virgl/fog-semantic.patch scripts/virgl/nvidia-vertex-binding.patch scripts/virgl/nvidia-vertex-binding-v1.patch
	python3 scripts/build-virgl-host.py
.PHONY: virgl-host
virgl-host: $(VIRGL_HOST_LIB)
RUN_GPU = $(if $(filter 1,$(RUN_VIRGL)),virtio-vga-gl,virtio-vga)
RUN_GL = $(if $(filter 1,$(RUN_VIRGL)),on,off)
# GTK's Wayland GL widget can lose relative input after its pointer grab.
# SDL preserves host relative reports with the same virgl device and USB mouse.
RUN_DISPLAY ?= $(if $(filter 1,$(RUN_VIRGL)),sdl,gtk)
# Preserve complete kernel hang reports independently of terminal scrollback.
# The stdio backend keeps the interactive serial commands (dump/diag/gpup).
RUN_SERIAL_LOG ?= build/logs/desktop-serial.log
.PHONY: run run-tcg
run: DISPLAY_OPT = -display $(RUN_DISPLAY),gl=$(RUN_GL),show-cursor=$(GTK_SHOW_CURSOR)$(if $(filter gtk,$(RUN_DISPLAY)),$(NET_COMMA)zoom-to-fit=off$(NET_COMMA)grab-on-hover=on)
run: QEMUFLAGS = $(QEMU_MEM) -vga none -device $(RUN_GPU),xres=1280,yres=800
run: edk2-ovmf $(IMAGE_NAME).iso $(RUN_DISK_DEP) $(if $(RUN_VIRGL_FIXED),$(VIRGL_HOST_LIB))
	@mkdir -p "$(dir $(RUN_SERIAL_LOG))"
	$(RUN_VIRGL_ENV) qemu-system-$(ARCH) \
		-M q35,pcspk-audiodev=snd0 \
		-drive if=pflash,unit=0,format=raw,file=edk2-ovmf/ovmf-code-$(ARCH).fd,readonly=on \
		-cdrom $(IMAGE_NAME).iso \
		-drive file=disk.img,format=raw,if=none,id=nvme0 \
		-device nvme,serial=avoryos0,drive=nvme0 \
		-cpu host -enable-kvm \
		-smp $(RUN_SMP) \
		-chardev stdio,id=desktop-serial,logfile=$(RUN_SERIAL_LOG),logappend=on \
		-serial chardev:desktop-serial \
		-audiodev pa,id=snd0,timer-period=2000,out.frequency=48000,out.channels=2,out.format=s16,out.buffer-length=$(AUDIO_BUFLEN),out.latency=$(AUDIO_LATENCY) \
		$(RUN_NET_FLAGS) \
		-device intel-hda -device hda-duplex,audiodev=snd0 \
		-device qemu-xhci,id=xhci \
		-device usb-kbd,bus=xhci.0 \
		-device $(RUN_POINTER),bus=xhci.0 \
		$(DISPLAY_OPT) \
		$(QEMUFLAGS)
run-tcg: run-x86_64

# ── NUMA multi-node test target ──────────────────────────────────────────────
#
# Boots AvoryOS with 2 NUMA nodes, 4 vCPUs (2 per node), 4GB RAM (2GB per node),
# and explicit inter-node distance matrix.
.PHONY: run-numa
run-numa: edk2-ovmf $(IMAGE_NAME).iso $(RUN_DISK_DEP)
	qemu-system-$(ARCH) \
		-M q35,pcspk-audiodev=snd0 \
		-drive if=pflash,unit=0,format=raw,file=edk2-ovmf/ovmf-code-$(ARCH).fd,readonly=on \
		-cdrom $(IMAGE_NAME).iso \
		-drive file=disk.img,format=raw,if=none,id=nvme0 \
		-device nvme,serial=avoryos0,drive=nvme0 \
		-cpu host -enable-kvm \
		-smp $(RUN_SMP),sockets=2,cores=4,threads=1 \
		-m 4G \
		-object memory-backend-ram,id=mem0,size=2G \
		-object memory-backend-ram,id=mem1,size=2G \
		-numa node,nodeid=0,cpus=0-3,memdev=mem0 \
		-numa node,nodeid=1,cpus=4-7,memdev=mem1 \
		-numa dist,src=0,dst=1,val=21 \
		-numa dist,src=1,dst=0,val=21 \
		-serial stdio \
		-audiodev pa,id=snd0,timer-period=2000,out.frequency=48000,out.channels=2,out.format=s16,out.buffer-length=$(AUDIO_BUFLEN),out.latency=$(AUDIO_LATENCY) \
		$(RUN_NET_FLAGS) \
		-device intel-hda -device hda-duplex,audiodev=snd0 \
		-device qemu-xhci,id=xhci \
		-device usb-kbd,bus=xhci.0 \
		-device usb-tablet,bus=xhci.0 \
		$(QEMUFLAGS)


# Shared QEMU settings for interactive and VFIO launchers.
SERIAL ?= stdio
# GTK cursor visibility can be overridden per boot. The guest draws its own
# framebuffer cursor when using a relative pointer.
GTK_SHOW_CURSOR ?= off
DISPLAY_OPT ?= -display gtk,zoom-to-fit=off,grab-on-hover=on,show-cursor=$(GTK_SHOW_CURSOR)
QEMU_MEM ?= -m 4G

VFIO_BDF ?= 0000:0e:00.0
VFIO_LAUNCHER ?= python3 scripts/vfio_resume_check.py --
VFIO_MEM ?= 4G
VFIO_SMP ?= 8
VFIO_ROM ?= build/vfio/vbios.rom
# Additional properties for the primary function.
VFIO_DEVICE_EXTRA ?=
VFIO_GUEST_ADDRESS ?= addr=03.0
VFIO_PREDEVICES ?=
# Extra QEMU -device arguments for a run-vfio boot, e.g.
#   make run-vfio VFIO_EXTRA='-device edu'
VFIO_EXTRA ?=
# Absolute pointing for ordinary VFIO desktops; PRIME games need relative motion.
VFIO_POINTER ?= usb-tablet
# Extra raw QEMU arguments for a run-vfio boot, e.g. to capture the exceptions
# around a silent triple-fault reset (the VM stops at the fault instead of
# rebooting):
#   make run-amd-gpu QEMU_EXTRA='-no-reboot -d int,cpu_reset,guest_errors -D build/logs/prime-qemu.log'
QEMU_EXTRA ?=
VFIO_COMMA := ,
VFIO_ROM_OPT = $(if $(wildcard $(VFIO_ROM)),$(VFIO_COMMA)romfile=$(VFIO_ROM),)
RUN_ISO ?= $(IMAGE_NAME).iso
RUN_ISO_DEP ?= $(RUN_ISO)

.PHONY: run-vfio
run-vfio: QEMUFLAGS = -vga none -device virtio-vga,addr=06.0,xres=1280,yres=800$(VFIO_VIRTIO_GPU_EXTRA)
run-vfio: edk2-ovmf $(RUN_ISO_DEP) $(RUN_DISK_DEP)
	$(VFIO_LAUNCHER) qemu-system-$(ARCH) \
		-M q35 \
		-m $(VFIO_MEM) \
		-cpu host -enable-kvm \
		-smp $(VFIO_SMP) \
		-drive if=pflash,unit=0,format=raw,file=edk2-ovmf/ovmf-code-$(ARCH).fd,readonly=on \
		-cdrom $(RUN_ISO) \
		-drive file=disk.img,format=raw,if=none,id=nvme0 \
		-device nvme,serial=avoryos0,drive=nvme0 \
		$(VFIO_PREDEVICES) \
		-device vfio-pci,host=$(VFIO_BDF),$(VFIO_GUEST_ADDRESS)$(VFIO_DEVICE_EXTRA),rombar=1$(VFIO_ROM_OPT) \
		-audiodev pa,id=snd0,timer-period=2000,out.frequency=48000,out.channels=2,out.format=s16,out.buffer-length=$(AUDIO_BUFLEN),out.latency=$(AUDIO_LATENCY) \
		-device intel-hda -device hda-duplex,audiodev=snd0 \
		-device qemu-xhci,id=xhci \
		-device usb-kbd,bus=xhci.0 \
		-device $(VFIO_POINTER),bus=xhci.0 \
		$(DISPLAY_OPT) \
		-serial $(SERIAL) \
		$(VFIO_EXTRA) \
		$(QEMU_EXTRA) \
		$(QEMUFLAGS)

# One-monitor VFIO mode: Xorg prefers AMD for its primary renderer and
# shares the desktop with the VirtIO output GPU. Keep Display Core enabled:
# modesetting needs AMD KMS resources even without a connected monitor.
.PHONY: run-amd-gpu
# Raphael recovery uses MODE2 together with checked PSP ring replacement.
# Both remain explicitly disableable for investigation.
KPI_AMDGPU_MODE2_RESET ?= 1
KPI_AMDGPU_PSP_REINIT ?= 1
run-amd-gpu: KERNEL_CMDLINE = kpi_amdgpu=1 kpi_prime_offload=1 kpi_amdgpu_mode2_reset=$(KPI_AMDGPU_MODE2_RESET) kpi_amdgpu_psp_reinit=$(KPI_AMDGPU_PSP_REINIT) amdgpu.runpm=0 amdgpu.ppfeaturemask=0xfff73fff drm.debug=0x4
run-amd-gpu: SERIAL = chardev:prime-diag
run-amd-gpu: VFIO_SMP = 8
# GLFW cursor capture/raw motion requires a relative device, not a tablet.
run-amd-gpu: VFIO_POINTER = usb-mouse
run-amd-gpu: GTK_SHOW_CURSOR = off
# GTK's X button closes QEMU directly. Recent X-close and Ctrl-C/reopen checks
# pass with the enabled recovery path; set GTK_WINDOW_CLOSE=off to require a
# guest shutdown before closing passthrough PRIME sessions.
GTK_WINDOW_CLOSE ?= on
run-amd-gpu: DISPLAY_OPT = -display gtk,zoom-to-fit=off,grab-on-hover=on,show-cursor=$(GTK_SHOW_CURSOR),window-close=$(GTK_WINDOW_CLOSE)
run-amd-gpu: VFIO_LAUNCHER = python3 scripts/qemu-prime-log.py --
run-amd-gpu: RUN_NIC_ADDR = 08.0
run-amd-gpu: VFIO_EXTRA = -device edu -chardev socket,id=prime-diag,path=build/logs/p6-amd-gpu-serial.sock,server=on,wait=off,logfile=build/logs/p6-amd-gpu.log,logappend=on $(RUN_NET_FLAGS)
run-amd-gpu: QEMUFLAGS = -vga none -device virtio-vga,addr=06.0,xres=1280,yres=800
run-amd-gpu: run-vfio

.PHONY: run-dist
run-dist: edk2-ovmf avoryos-dist.iso
	qemu-system-$(ARCH) \
		-M q35 \
		-drive if=pflash,unit=0,format=raw,file=edk2-ovmf/ovmf-code-$(ARCH).fd,readonly=on \
		-cdrom avoryos-dist.iso \
		-m 2G \
		-serial stdio \
		$(QEMUFLAGS)

avoryos-dist.iso: limine/limine kernel disk.img limine.conf create_dist_usb.sh
	./create_dist_usb.sh avoryos-dist.iso

# ── Minimal ISO ─────────────────────────────────────────────────────────────
# bash + GNU coreutils + Xorg + IceWM + BusyBox init/D-Bus + xterm, without the full
# desktop stack.  scripts/build-minimal.sh assembles an independent rootfs and
# disk image under build/minimal/; create_dist_usb.sh packages it as a
# self-contained ISO (the disk image is embedded as a Limine module, exactly
# like avoryos-dist.iso).
MINIMAL_ISO := avoryos-minimal.iso
MINIMAL_DISK := build/minimal/disk.img
MINIMAL_STAMP := build/minimal/rootfs/.avoryos-minimal

$(MINIMAL_STAMP): scripts/build-minimal.sh scripts/axon-common.sh scripts/minimal-rootfs-packages.txt
	chmod +x scripts/build-minimal.sh
	./scripts/build-minimal.sh rootfs

.PHONY: minimal-rootfs
minimal-rootfs: $(MINIMAL_STAMP)

$(MINIMAL_DISK): $(MINIMAL_STAMP) scripts/build-minimal.sh
	./scripts/build-minimal.sh disk

.PHONY: minimal-disk
minimal-disk: $(MINIMAL_DISK)

$(MINIMAL_ISO): kernel limine/limine $(MINIMAL_DISK) limine-minimal.conf create_dist_usb.sh
	DISK_IMG=$(MINIMAL_DISK) LIMINE_CONF=limine-minimal.conf \
		./create_dist_usb.sh $(MINIMAL_ISO)

.PHONY: minimal-iso
minimal-iso: $(MINIMAL_ISO)

.PHONY: run-minimal
run-minimal: edk2-ovmf $(MINIMAL_ISO)
	qemu-system-$(ARCH) \
		-M q35,pcspk-audiodev=snd0 \
		-drive if=pflash,unit=0,format=raw,file=edk2-ovmf/ovmf-code-$(ARCH).fd,readonly=on \
		-cdrom $(MINIMAL_ISO) \
		-serial stdio \
		$(RUN_NET_FLAGS) \
		-device qemu-xhci,id=xhci \
		-device usb-kbd,bus=xhci.0 \
		-device usb-tablet,bus=xhci.0 \
		$(QEMUFLAGS)

.PHONY: clean-minimal
clean-minimal:
	rm -rf build/minimal $(MINIMAL_ISO)

# Legacy SATA/IDE root disk (the pre-NVMe run layout), kept for AHCI/ATA
# regression and A/B comparisons.  run-sata is KVM, run-sata-tcg is TCG.
.PHONY: run-sata run-sata-tcg
run-sata: edk2-ovmf $(IMAGE_NAME).iso $(RUN_DISK_DEP)
	qemu-system-$(ARCH) \
		-M q35,pcspk-audiodev=snd0 \
		-drive if=pflash,unit=0,format=raw,file=edk2-ovmf/ovmf-code-$(ARCH).fd,readonly=on \
		-cdrom $(IMAGE_NAME).iso \
		-drive file=disk.img,format=raw,if=none,id=disk0 -device ide-hd,drive=disk0,bus=ide.0 \
		-cpu host -enable-kvm \
		-smp 4 \
		-serial stdio \
		-audiodev pa,id=snd0,timer-period=2000,out.frequency=48000,out.channels=2,out.format=s16,out.buffer-length=$(AUDIO_BUFLEN),out.latency=$(AUDIO_LATENCY) \
		$(RUN_NET_FLAGS) \
		-device intel-hda -device hda-duplex,audiodev=snd0 \
		-device qemu-xhci,id=xhci \
		-device usb-kbd,bus=xhci.0 \
		-device usb-tablet,bus=xhci.0 \
		$(QEMUFLAGS)

.PHONY: run-bios
run-bios: $(IMAGE_NAME).iso $(RUN_DISK_DEP)
	qemu-system-$(ARCH) \
		-M q35,pcspk-audiodev=snd0 \
		-cdrom $(IMAGE_NAME).iso \
		-hda disk.img \
		-boot d \
		-audiodev pa,id=snd0 \
		-serial stdio \
		-device AC97,audiodev=snd0 \
		-device intel-hda -device hda-duplex,audiodev=snd0 \
		-device usb-ehci,id=ehci \
		-device usb-tablet,bus=ehci.0 \
		-device usb-kbd,bus=ehci.0 \
		$(QEMUFLAGS)

.PHONY: run-ata
run-ata: edk2-ovmf $(IMAGE_NAME).iso $(RUN_DISK_DEP)
	qemu-system-$(ARCH) \
		-M pc,pcspk-audiodev=snd0 \
		-drive if=pflash,unit=0,format=raw,file=edk2-ovmf/ovmf-code-$(ARCH).fd,readonly=on \
		-cdrom $(IMAGE_NAME).iso \
		-drive file=disk.img,format=raw,if=ide \
		-smp 4 \
		-serial stdio \
		-audiodev pa,id=snd0 \
		-device sb16,audiodev=snd0 \
		-device AC97,audiodev=snd0 \
		-device intel-hda -device hda-duplex,audiodev=snd0 \
		-device usb-ehci,id=ehci \
		-device usb-tablet,bus=ehci.0 \
		-device usb-kbd,bus=ehci.0 \
		$(QEMUFLAGS)

# FAT32 test image for testing the FAT32 driver
nvme_test.img: scripts/nvme/create-nvme-test.sh userland/nvme_init.elf \
		userland/shutdown.elf userland/nvme_test.elf userland/nvme_bench.elf
	./scripts/nvme/create-nvme-test.sh

fat32_test.img:
	./scripts/create-fat32-test.sh

.PHONY: run-fat32
run-fat32: edk2-ovmf $(IMAGE_NAME).iso fat32_test.img
	qemu-system-$(ARCH) \
		-M q35,pcspk-audiodev=snd0 \
		-drive if=pflash,unit=0,format=raw,file=edk2-ovmf/ovmf-code-$(ARCH).fd,readonly=on \
		-cdrom $(IMAGE_NAME).iso \
		-hda fat32_test.img \
		-smp 4 \
		-serial stdio \
		-audiodev pa,id=snd0 \
		-device sb16,audiodev=snd0 \
		-device AC97,audiodev=snd0 \
		-device intel-hda -device hda-duplex,audiodev=snd0 \
		-device qemu-xhci,id=xhci \
		-device usb-kbd,bus=xhci.0 \
		-device usb-tablet,bus=xhci.0 \
		$(QEMUFLAGS)

USERLAND_DISK_APPS := \
	userland/asplay.elf \
	userland/avory-installer.elf \
	userland/avory-login.elf \
	userland/booter.elf \
	userland/classicube.elf \
	userland/dns_lookup.elf \
	userland/drm_identify.elf \
	userland/hello_glibc.elf \
	userland/kilo.elf \
	userland/kria.elf \
	userland/ls.elf \
	userland/lsblk.elf \
	userland/lspci.elf \
	userland/pong.elf \
	userland/proc_bench.elf \
	userland/radeonsi_vram_leak.elf \
	userland/raycast.elf \
	userland/readelf.elf \
	userland/reboot.elf \
	userland/shutdown.elf \
	userland/test_blocking_liveness.elf \
	userland/test_child_notify.elf \
	userland/test_clone_futex.elf \
	userland/test_copy_user.elf \
	userland/test_dcache.elf \
	userland/test_elf32.elf \
	userland/test_fb_perf.elf \
	userland/test_futex_clock.elf \
	userland/test_futex_pi.elf \
	userland/test_heap_smp.elf \
	userland/test_hugepages.elf \
	userland/test_journal_speed.elf \
	userland/test_kpi_amdgpu.elf \
	userland/test_kpi_bochs.elf \
	userland/test_kpi_dmabuf.elf \
	userland/test_kpi_drm.elf \
	userland/test_kpi_radeonsi.elf \
	userland/test_lazy_fpu.elf \
	userland/test_mem_stress.elf \
	userland/test_mm_speed.elf \
	userland/test_pcp_pmm.elf \
	userland/test_pty_master.elf \
	userland/test_signal_stack.elf \
	userland/test_syscall_cpu.elf \
	userland/test_syscall_speed.elf \
	userland/test_syscompat.elf \
	userland/test_ticket_lock.elf \
	userland/test_tlb_stress.elf \
	userland/test_tmpfile.elf \
	userland/test_uaccess_bench.elf \
	userland/test_unix_sockets.elf \
	userland/test_user_dispatch.elf \
	userland/test_vdso_bench.elf \
	userland/test_watchdog.elf \
	userland/test_zero_page.elf

disk.img: $(USERLAND_DISK_APPS)

# Create a 5GB ext4 disk image with sample files for testing
# Deterministic 4 KB firmware blob read by the Phase 5 firmware self-test
# (byte i = (i * 7 + 3) & 0xff).  Staged into the rootfs /lib/firmware below.
build/test_fw.bin:
	@mkdir -p build
	@python3 -c 'import sys; open(sys.argv[1], "wb").write(bytes(((i * 7 + 3) & 0xff) for i in range(4096)))' $@

# Phase 6 C4: amdgpu firmware blobs.  scripts/linux-firmware-install.sh follows
# scripts/linux/firmware-manifest.txt and stages the pinned linux-firmware ref
# (the script's default; override with LINUX_FIRMWARE_REF=).  Point
# LINUX_FIRMWARE_SRC=/lib/firmware at a distro firmware tree to skip the git
# checkout for local runs; .zst/.xz members are decompressed during staging.
LINUX_FIRMWARE_REF ?= 20260916
build/firmware/.stamp: scripts/linux-firmware-install.sh scripts/linux/firmware-manifest.txt scripts/linux/firmware-sha256-20260916.txt
	LINUX_FIRMWARE_REF='$(LINUX_FIRMWARE_REF)' scripts/linux-firmware-install.sh
	@touch $@

disk.img: userland/winoptions userland/icewm-menu $(ARCH_STAMP)
disk.img: scripts/configure-accounts.sh userland/avory-account userland/test_accounts.sh
# Fixed 12 GiB image by default (override during a forced rebuild with
# `make disk-rebuild DISK_SIZE_MIB=<MiB>`). Leave room for filesystem metadata
# and tools added after the rootfs overlay.
DISK_SIZE_MIB ?= 12288
DISK_PARTITION_CACHE := build/arch/disk-partition.img
DISK_APP_MANIFEST := build/arch/disk-apps.json
DISK_NON_APP_DEPS := GNUmakefile \
	userland/winoptions userland/icewm-menu $(ARCH_STAMP) \
	scripts/setup-rootfs.sh scripts/axon-common.sh scripts/configure-axon-mirrors.py \
	scripts/rootfs-packages.txt $(AXON_PROFILE_FILES) userland/avory-login.c \
	scripts/glibc-toolchain.sh $(GLIBC_TOOLCHAIN_STAMP) \
	scripts/configure-accounts.sh userland/avory-account userland/test_accounts.sh \
	scripts/populate-ext2-dir.sh scripts/configure-image-root.sh config/init/rcS \
	config/plasma/avory-plasma-wayland.sh \
	config/weston/avory-weston-session.sh config/weston/weston.desktop \
	config/xorg/10-modesetting.conf config/xorg/10-native.conf config/xorg/10-virgl.conf config/xorg/20-output.conf \
	scripts/image-runtime.py config/xorg/avory-prime-display.sh \
	config/xorg/45-prime-output.sh config/xorg/avory-xorg-profile.sh \
	config/xorg/10-nouveau.conf config/xorg/avory-nouveau-xorg.sh \
	config/xorg/44-nouveau-env.sh scripts/image-firmware-install.sh \
	scripts/image-add-firmware.py scripts/incremental-disk-apps.py \
	scripts/linux-firmware-install.sh scripts/linux/firmware-manifest.txt \
	scripts/linux/firmware-sha256-20260916.txt build/test_fw.bin build/firmware/.stamp \
	$(QUAKE2_STAMP) scripts/build-quake2.sh scripts/quake2-sdl2-config.in \
	$(QUAKE2_BUNDLE_FILES) \
	assets/boot.wav assets/test.wav assets/jane.mp3 assets/mc9.mp3 \
	assets/train.mp3 assets/test.bmp assets/test.tar assets/room.png \
	assets/logo.png assets/linus.gif assets/video.mp4 userland/terrain.png \
	userland/texpacks/classicube.zip initrd/weston.ini initrd/drm-pick.sh \
	initrd/avory-drm.sh

# Userland app edits update only their installed files in the cached ext4
# partition. Changes to rootfs/config/assets still take the safe full-build
# path. A clean checkout also takes the full path to create the cache.
DISK_REBUILD ?= $(shell \
	if [ ! -f disk.img ] || [ ! -f $(DISK_PARTITION_CACHE) ] || [ ! -f $(DISK_APP_MANIFEST) ]; then echo 1; exit; fi; \
	for path in $(DISK_NON_APP_DEPS); do \
		if [ ! -e "$$path" ] || [ "$$path" -nt disk.img ]; then echo 1; exit; fi; \
	done; echo 0)

disk.img: scripts/populate-ext2-dir.sh scripts/configure-image-root.sh
# All Xorg profiles ship in every image; /sbin/avory-early-init selects the
# one matching the boot's PCI topology (amdgpu passthrough vs native card0),
# so bare `sddm` works with or without VFIO. XORG_PROFILE only sets the
# baked default for boots where detection is inconclusive.
XORG_PROFILE ?= amdgpu
XORG_CONFIG = $(if $(filter amdgpu,$(XORG_PROFILE)),config/xorg/10-modesetting.conf,$(if $(filter virgl,$(XORG_PROFILE)),config/xorg/10-virgl.conf,config/xorg/10-native.conf))

# Pick a profile when populating a disk. Switching launch targets never
# needs a rebuild: early init re-selects the profile on every boot.
disk.img: config/xorg/10-modesetting.conf config/xorg/10-native.conf config/xorg/10-virgl.conf config/xorg/20-output.conf config/kwinoutputconfig.json scripts/image-runtime.py
disk.img: config/xorg/avory-prime-display.sh config/xorg/45-prime-output.sh config/xorg/avory-xorg-profile.sh
disk.img: config/xorg/10-nouveau.conf config/xorg/avory-nouveau-xorg.sh
disk.img: config/xorg/44-nouveau-env.sh
disk.img: scripts/image-firmware-install.sh
disk.img: build/test_fw.bin build/firmware/.stamp
disk.img: $(ARCH_STAMP) $(QUAKE2_BUNDLE_FILES)
disk.img: assets/boot.wav assets/test.wav assets/jane.mp3 assets/mc9.mp3 assets/train.mp3 assets/test.bmp assets/test.tar assets/room.png assets/logo.png assets/linus.gif assets/video.mp4 userland/terrain.png userland/texpacks/classicube.zip initrd/weston.ini initrd/drm-pick.sh initrd/avory-drm.sh

disk.img: GNUmakefile scripts/incremental-disk-apps.py

.PHONY: disk-rebuild disk-full-force
disk-full-force:

disk-rebuild:
	$(MAKE) --no-print-directory DISK_REBUILD=1 disk.img

ifeq ($(DISK_REBUILD),1)
disk.img: disk-full-force

	@echo "Creating partitioned disk image (GPT)..."
	@if ls -l /proc/[0-9]*/fd/* 2>/dev/null | grep -q "disk.img"; then \
		echo "Refusing: disk.img is open by a running process (QEMU?). Stop the VM before rebuilding the disk image; rebuilding it in place corrupts the guest's filesystem." >&2; \
		exit 1; \
	fi
	@if _rootfs_mb=$$(du -sm build/arch/rootfs 2>/dev/null | cut -f1); \
		[ -n "$$_rootfs_mb" ] && [ "$$_rootfs_mb" -gt "$$(( $(DISK_SIZE_MIB) - 1024 ))" ]; then \
		echo "Refusing: rootfs ($${_rootfs_mb}MiB) exceeds $(DISK_SIZE_MIB)MiB minus 1GiB metadata headroom; rebuild with DISK_SIZE_MIB=<MiB>." >&2; \
		exit 1; \
	fi
	@if [ -L ./part.img ]; then \
		old_loop=$$(readlink ./part.img); \
		case "$$old_loop" in /dev/loop*) losetup -d "$$old_loop" >/dev/null 2>&1 || true;; esac; \
	fi
	rm -f ./part.img
	truncate -s $(DISK_SIZE_MIB)M disk.img
	printf 'label: gpt\nstart=2048, size=%s, type=4F68BCE3-E8CD-4DB1-96E7-FBCAF984B709, name="rootfs"\n' $$(( ($(DISK_SIZE_MIB) - 2) * 2048 )) | sfdisk disk.img >/dev/null 2>&1 || (parted -s disk.img mklabel gpt && parted -s disk.img mkpart rootfs ext4 1MiB $$(( $(DISK_SIZE_MIB) - 1 ))MiB)
	@if loop=$$(losetup --find --show --offset 1048576 --sizelimit $$(( ($(DISK_SIZE_MIB) - 2) * 1048576 )) disk.img 2>/dev/null); then \
		ln -s "$$loop" ./part.img; \
		echo "Using partition loop device $$loop (formatting in place)"; \
	else \
		echo "Loop device unavailable; using sparse temporary partition image"; \
		truncate -s $$(( $(DISK_SIZE_MIB) - 2 ))M ./part.img; \
	fi
	@echo "Creating root filesystem (ext4)..."
	# The kernel's VFS uses 32-bit file sizes/offsets; avoid the host default
	# large_file feature so the generated root stays writable under this kernel.
	mkfs.ext4 -F -b 1024 -I 128 \
		-O extent,filetype,has_journal,dir_index,^64bit,^metadata_csum,^flex_bg,^huge_file,^large_file,^dir_nlink,^extra_isize,^metadata_csum_seed,^orphan_file \
		./part.img
	@echo "Populating root filesystem..."
	@{ \
		echo "cd /"; \
		echo "mkdir tmp"; \
		echo "mkdir bin"; \
		echo "mkdir .config"; \
		echo "mkdir .cache"; \
		echo "rm bin/startx.sh"; \
		echo "rm bin/startw.sh"; \
		echo "rm bin/avory-login"; \
		echo "write userland/avory-login.elf bin/avory-login"; \
		echo "rm bin/avory-installer"; \
		echo "write userland/avory-installer.elf bin/avory-installer"; \
		echo "rm usr/sbin/avory-installer"; \
		echo "write userland/avory-installer.elf usr/sbin/avory-installer"; \
		echo "rm bin/install-avoryos"; \
		echo "symlink bin/install-avoryos /usr/sbin/avory-installer"; \
		echo "rm bin/false"; \
		echo "symlink bin/false /usr/bin/false"; \
		echo "rm bin/true"; \
		echo "symlink bin/true /usr/bin/true"; \
		echo "mkdir etc"; \
		echo "rm etc/weston.ini"; \
		echo "write initrd/weston.ini etc/weston.ini"; \
	} | debugfs -w ./part.img >/dev/null 2>&1 || true
	@if [ -d build/arch/rootfs/usr/include ]; then \
		echo "Installing GLIBC headers into disk image (opt/glibc/include)..."; \
		./scripts/populate-ext2-dir.sh ./part.img build/arch/rootfs/usr/include opt/glibc/include; \
	fi
	# NOTE: the compiler bundle's glibc *libs* are deliberately NOT copied.
	# The Axon rootfs supplies glibc 2.44, and overlaying the compiler bundle's
	# older sysroot on top mixes two glibc versions in /usr/lib,
	# which breaks every dynamically linked binary (e.g. bash failing on
	# __pointer_chk_guard). This step only made sense for the old musl tree.
	@{ \
		echo "cd /"; \
		echo "rm bin/netlink_test"; \
		echo "write userland/netlink_test.elf bin/netlink_test"; \
		echo "rm bin/kilo"; \
		echo "write userland/kilo.elf bin/kilo"; \
		echo "rm bin/hello_glibc"; \
		echo "write userland/hello_glibc.elf bin/hello_glibc"; \
		echo "rm bin/ls"; \
		echo "write userland/ls.elf bin/ls"; \
		echo "rm bin/lspci"; \
		echo "write userland/lspci.elf bin/lspci"; \
		echo "rm bin/lsblk"; \
		echo "write userland/lsblk.elf bin/lsblk"; \
		echo "rm bin/readelf"; \
		echo "write userland/readelf.elf bin/readelf"; \
		echo "rm bin/pong"; \
		echo "write userland/pong.elf bin/pong"; \
		echo "rm bin/raycast"; \
		echo "write userland/raycast.elf bin/raycast"; \
		echo "rm bin/kria"; \
		echo "write userland/kria.elf bin/kria"; \
		echo "rm bin/asplay"; \
		echo "write userland/asplay.elf bin/asplay"; \
		echo "rm bin/booter"; \
		echo "write userland/booter.elf bin/booter"; \
		echo "rm bin/reboot"; \
		echo "write userland/reboot.elf bin/reboot"; \
		echo "rm bin/shutdown"; \
		echo "write userland/shutdown.elf bin/shutdown"; \
		echo "rm bin/test_cred"; \
		echo "write userland/test_cred.elf bin/test_cred"; \
		echo "rm standalone.s"; \
		echo "write userland/standalone.s standalone.s"; \
		echo "rm test.wav"; \
		echo "write assets/test.wav test.wav"; \
		echo "rm boot.wav"; \
		echo "write assets/boot.wav boot.wav"; \
		echo "rm jane.mp3"; \
		echo "write assets/jane.mp3 jane.mp3"; \
		echo "rm mc9.mp3"; \
		echo "write assets/mc9.mp3 mc9.mp3"; \
		echo "rm train.mp3"; \
		echo "write assets/train.mp3 train.mp3"; \
		echo "rm test.bmp"; \
		echo "write assets/test.bmp test.bmp"; \
		echo "mkdir assets"; \
		echo "rm assets/room.png"; \
		echo "write assets/room.png assets/room.png"; \
		echo "rm assets/logo.png"; \
		echo "write assets/logo.png assets/logo.png"; \
		echo "rm assets/linus.gif"; \
		echo "write assets/linus.gif assets/linus.gif"; \
		echo "rm assets/video.mp4"; \
		echo "write assets/video.mp4 assets/video.mp4"; \
		echo "rm hello.krx"; \
		echo "write userland/hello.krx hello.krx"; \
		echo "rm bin/dns_lookup"; \
		echo "write userland/dns_lookup.elf bin/dns_lookup"; \
		echo "rm test.tar"; \
		echo "write assets/test.tar test.tar"; \
		echo "rm bin/test_mem_stress"; \
		echo "write userland/test_mem_stress.elf bin/test_mem_stress"; \
		echo "rm bin/test_clone_futex"; \
		echo "write userland/test_clone_futex.elf bin/test_clone_futex"; \
		echo "rm bin/test_signal_stack"; \
		echo "write userland/test_signal_stack.elf bin/test_signal_stack"; \
		echo "rm bin/test_user_dispatch"; \
		echo "write userland/test_user_dispatch.elf bin/test_user_dispatch"; \
		echo "rm bin/test_tlb_stress"; \
		echo "write userland/test_tlb_stress.elf bin/test_tlb_stress"; \
		echo "rm bin/test_futex_clock"; \
		echo "write userland/test_futex_clock.elf bin/test_futex_clock"; \
		echo "rm bin/test_syscompat"; \
		echo "write userland/test_syscompat.elf bin/test_syscompat"; \
		echo "rm bin/test_futex_pi"; \
		echo "write userland/test_futex_pi.elf bin/test_futex_pi"; \
		echo "rm bin/test_unix_sockets"; \
		echo "write userland/test_unix_sockets.elf bin/test_unix_sockets"; \
		echo "rm bin/test_tmpfile"; \
		echo "write userland/test_tmpfile.elf bin/test_tmpfile"; \
		echo "rm bin/test_kpi_dmabuf"; \
		echo "write userland/test_kpi_dmabuf.elf bin/test_kpi_dmabuf"; \
		echo "rm bin/test_kpi_drm"; \
		echo "write userland/test_kpi_drm.elf bin/test_kpi_drm"; \
		echo "rm bin/test_kpi_bochs"; \
		echo "write userland/test_kpi_bochs.elf bin/test_kpi_bochs"; \
		echo "rm bin/test_kpi_amdgpu"; \
		echo "write userland/test_kpi_amdgpu.elf bin/test_kpi_amdgpu"; \
		echo "rm bin/test_kpi_radeonsi"; \
		echo "write userland/test_kpi_radeonsi.elf bin/test_kpi_radeonsi"; \
		echo "rm bin/test_child_notify"; \
		echo "write userland/test_child_notify.elf bin/test_child_notify"; \
		echo "rm bin/test_pty_master"; \
		echo "write userland/test_pty_master.elf bin/test_pty_master"; \
		echo "rm bin/test_syscall_speed"; \
		echo "write userland/test_syscall_speed.elf bin/test_syscall_speed"; \
		echo "rm bin/test_mm_speed"; \
		echo "write userland/test_mm_speed.elf bin/test_mm_speed"; \
		echo "rm bin/test_journal_speed"; \
		echo "write userland/test_journal_speed.elf bin/test_journal_speed"; \
		echo "rm bin/test_syscall_cpu"; \
		echo "write userland/test_syscall_cpu.elf bin/test_syscall_cpu"; \
		echo "rm bin/test_blocking_liveness"; \
		echo "write userland/test_blocking_liveness.elf bin/test_blocking_liveness"; \
		echo "rm bin/test_hugepages"; \
		echo "write userland/test_hugepages.elf bin/test_hugepages"; \
		echo "rm bin/test_zero_page"; \
		echo "write userland/test_zero_page.elf bin/test_zero_page"; \
		echo "rm bin/test_copy_user"; \
		echo "write userland/test_copy_user.elf bin/test_copy_user"; \
		echo "rm bin/test_pcp_pmm"; \
		echo "write userland/test_pcp_pmm.elf bin/test_pcp_pmm"; \
		echo "rm bin/test_ticket_lock"; \
		echo "write userland/test_ticket_lock.elf bin/test_ticket_lock"; \
		echo "rm bin/test_fb_perf"; \
		echo "write userland/test_fb_perf.elf bin/test_fb_perf"; \
		echo "rm bin/test_vdso_bench"; \
		echo "write userland/test_vdso_bench.elf bin/test_vdso_bench"; \
		echo "rm bin/test_lazy_fpu"; \
		echo "write userland/test_lazy_fpu.elf bin/test_lazy_fpu"; \
		echo "rm bin/test_elf32"; \
		echo "write userland/test_elf32.elf bin/test_elf32"; \
		echo "rm bin/test_heap_smp"; \
		echo "write userland/test_heap_smp.elf bin/test_heap_smp"; \
		echo "rm bin/test_dcache"; \
		echo "write userland/test_dcache.elf bin/test_dcache"; \
		echo "rm bin/test_uaccess_bench"; \
		echo "write userland/test_uaccess_bench.elf bin/test_uaccess_bench"; \
		echo "rm bin/test_watchdog"; \
		echo "write userland/test_watchdog.elf bin/test_watchdog"; \
		echo "rm bin/test_readahead"; \
		echo "write userland/test_readahead.elf bin/test_readahead"; \
		echo "rm bin/proc_bench"; \
		echo "rm bin/radeonsi_vram_leak.elf"; \
		echo "write userland/radeonsi_vram_leak.elf bin/radeonsi_vram_leak.elf"; \
		echo "write userland/proc_bench.elf bin/proc_bench"; \
		echo "rm bin/classicube"; \
		echo "write userland/classicube.elf bin/classicube"; \
		echo "mkdir texpacks"; \
		echo "rm terrain.png"; \
		echo "write userland/terrain.png terrain.png"; \
		echo "rm texpacks/classicube.zip"; \
		echo "write userland/texpacks/classicube.zip texpacks/classicube.zip"; \
		echo "rm texpacks/default.zip"; \
		echo "write userland/texpacks/classicube.zip texpacks/default.zip"; \
		echo "rm bin/fault_mon"; \
		echo "write userland/fault_mon.elf bin/fault_mon"; \
	} | debugfs -w ./part.img >/dev/null 2>&1 || true






	@echo "Writing ClassiCube options.txt (texture pack config)..."
	@printf 'texture-pack=classicube.zip\nskin-server=\n' > /tmp/classicube_options.txt
	debugfs -w -R "rm options.txt" ./part.img >/dev/null 2>&1 || true
	debugfs -w -R "write /tmp/classicube_options.txt options.txt" ./part.img >/dev/null 2>&1 || true
	rm -f /tmp/classicube_options.txt
	rm -f /tmp/avoryos_hello.txt /tmp/avoryos_readme.txt
	# The *.a trim (populate-ext2-dir.sh) is overridden for targeted trees
	# below: an in-image gcc link needs usr/include, the gcc static libs
	# (libgcc.a/libstdc++.a have no dynamic counterpart) and glibc's
	# *_nonshared.a archives, which /usr/lib/libc.so's ld script GROUPs in.
	@if [ -d build/arch/rootfs ]; then \
		mkdir -p build/arch/rootfs/lib/firmware; \
		cp -f build/test_fw.bin build/arch/rootfs/lib/firmware/test_fw.bin; \
		if [ -d build/firmware/lib/firmware ]; then \
			cp -a build/firmware/lib/firmware/. build/arch/rootfs/lib/firmware/; \
		fi; \
	fi
	@if [ -d build/arch/rootfs ]; then \
		echo "Populating Arch Linux rootfs into disk image..."; \
		mkdir -p build/arch/rootfs/etc/X11/xorg.conf.d; \
		mkdir -p build/arch/rootfs/usr/share/avory-xorg; \
		install -m 0644 config/xorg/10-modesetting.conf build/arch/rootfs/usr/share/avory-xorg/10-modesetting-amdgpu.conf; \
		install -m 0644 config/xorg/10-native.conf build/arch/rootfs/usr/share/avory-xorg/10-modesetting-native.conf; \
		install -m 0644 config/xorg/10-virgl.conf build/arch/rootfs/usr/share/avory-xorg/10-modesetting-virgl.conf; \
		install -m 0644 $(XORG_CONFIG) build/arch/rootfs/etc/X11/xorg.conf.d/10-modesetting.conf; \
		install -m 0755 config/xorg/avory-xorg-profile.sh build/arch/rootfs/usr/bin/avory-xorg-profile; \
		install -m 0755 config/xorg/avory-nouveau-xorg.sh build/arch/rootfs/usr/bin/avory-nouveau-xorg; \
		install -m 0644 config/xorg/10-nouveau.conf build/arch/rootfs/usr/share/avory-xorg/10-modesetting-nouveau.conf; \
		install -m 0755 config/xorg/avory-prime-display.sh build/arch/rootfs/usr/bin/avory-prime-display; \
		if [ -f build/arch/rootfs/sbin/avory-early-init ]; then \
			grep -q 'avory-xorg-profile' build/arch/rootfs/sbin/avory-early-init || \
				echo '/usr/bin/avory-xorg-profile 2>/dev/null || true' >> build/arch/rootfs/sbin/avory-early-init; \
		else \
			{ echo '#!/bin/sh'; echo '/usr/bin/avory-xorg-profile 2>/dev/null || true'; } > build/arch/rootfs/sbin/avory-early-init; \
			chmod +x build/arch/rootfs/sbin/avory-early-init; \
		fi; \
		mkdir -p build/arch/rootfs/etc/X11/xinit/xinitrc.d build/arch/rootfs/etc/profile.d; \
		install -m 0755 config/xorg/44-nouveau-env.sh build/arch/rootfs/etc/X11/xinit/xinitrc.d/44-nouveau-env.sh; \
		install -m 0644 config/xorg/44-nouveau-env.sh build/arch/rootfs/etc/profile.d/avory-nouveau.sh; \
		install -m 0755 config/xorg/45-prime-output.sh build/arch/rootfs/etc/X11/xinit/xinitrc.d/45-prime-output.sh; \
		mkdir -p build/arch/rootfs/etc/sddm.conf.d; \
		install -m 0644 config/xorg/20-output.conf build/arch/rootfs/etc/sddm.conf.d/20-output.conf; \
		if [ -f build/arch/rootfs/etc/sddm.conf ]; then \
			sed -i 's|^DisplayCommand=.*$$|DisplayCommand=/usr/bin/avory-prime-display|' build/arch/rootfs/etc/sddm.conf; \
		fi; \
		install -m 0755 config/init/rcS build/arch/rootfs/etc/init.d/rcS; \
		install -m 0755 config/weston/avory-weston-session.sh build/arch/rootfs/usr/bin/avory-weston-session; \
		install -m 0644 config/weston/weston.desktop build/arch/rootfs/usr/share/wayland-sessions/weston.desktop; \
		./scripts/configure-accounts.sh build/arch/rootfs; \
		./scripts/populate-ext2-dir.sh ./part.img build/arch/rootfs /; \
		EXCLUDE_IMAGE_STATIC="" ./scripts/populate-ext2-dir.sh ./part.img build/arch/rootfs/usr/include usr/include; \
		EXCLUDE_IMAGE_STATIC="" ./scripts/populate-ext2-dir.sh ./part.img build/arch/rootfs/usr/lib/gcc/x86_64-pc-linux-gnu usr/lib/gcc/x86_64-pc-linux-gnu; \
		mkdir -p build/image-toolchain/usr/lib; \
		cp -a build/arch/rootfs/usr/lib/*_nonshared.a build/image-toolchain/usr/lib/ 2>/dev/null || true; \
		EXCLUDE_IMAGE_STATIC="" ./scripts/populate-ext2-dir.sh ./part.img build/image-toolchain /; \
	fi
	@python3 scripts/image-runtime.py ./part.img build/arch/rootfs \
		/usr/lib/libc.so.6 /usr/lib/ld-linux-x86-64.so.2
	# The Arch rootfs may still carry a stale bin/startx.sh; drop any copies
	# of the retired direct-boot scripts and restore the AvoryOS-owned files
	# after the overlay, otherwise XFCE runs startxfce4 and opens its
	# failsafe-session error dialog.
	@{ \
		echo "cd /"; \
		echo "rm bin/startx.sh"; \
		echo "rm bin/startw.sh"; \
		echo "rm bin/drm-pick.sh"; \
		echo "write initrd/drm-pick.sh bin/drm-pick.sh"; \
		echo "rm bin/drm-identify"; \
		echo "write userland/drm_identify.elf bin/drm-identify"; \
		echo "set_inode_field bin/drm-identify mode 0100755"; \
		echo "rm etc/profile.d/avory-drm.sh"; \
		echo "write initrd/avory-drm.sh etc/profile.d/avory-drm.sh"; \
		echo "mkdir etc/xdg/plasma-workspace"; \
		echo "mkdir etc/xdg/plasma-workspace/env"; \
		echo "rm etc/xdg/plasma-workspace/env/avory-drm.sh"; \
		echo "write initrd/avory-drm.sh etc/xdg/plasma-workspace/env/avory-drm.sh"; \
		echo "set_inode_field etc/xdg/plasma-workspace/env/avory-drm.sh mode 0100644"; \
		echo "rm etc/weston.ini"; \
		echo "write initrd/weston.ini etc/weston.ini"; \
		echo "set_inode_field bin/drm-pick.sh mode 0100755"; \
		echo "set_inode_field etc/profile.d/avory-drm.sh mode 0100644"; \
		echo "set_inode_field var/lib/sddm mode 040775"; \
		_sddm_u=$$(awk -F: '/^sddm:/{print $$3; exit}' build/arch/rootfs/etc/passwd 2>/dev/null); \
		_sddm_g=$$(awk -F: '/^sddm:/{print $$3; exit}' build/arch/rootfs/etc/group 2>/dev/null); \
		[ -n "$$_sddm_u" ] || _sddm_u=642; \
		[ -n "$$_sddm_g" ] || _sddm_g=642; \
		echo "set_inode_field var/lib/sddm uid $$_sddm_u"; \
		echo "set_inode_field var/lib/sddm gid $$_sddm_g"; \
		echo "set_inode_field var/lib/sddm/.config mode 040775"; \
		echo "set_inode_field var/lib/sddm/.config uid $$_sddm_u"; \
		echo "set_inode_field var/lib/sddm/.config gid $$_sddm_g"; \
		echo "set_inode_field var/lib/sddm/.config/sddm-greeter-qt6rc mode 0100664"; \
		echo "set_inode_field var/lib/sddm/.config/sddm-greeter-qt6rc uid $$_sddm_u"; \
		echo "set_inode_field var/lib/sddm/.config/sddm-greeter-qt6rc gid $$_sddm_g"; \
		echo "set_inode_field var/lib/sddm/.config/sddm-greeter-qt5rc mode 0100664"; \
		echo "set_inode_field var/lib/sddm/.config/sddm-greeter-qt5rc uid $$_sddm_u"; \
		echo "set_inode_field var/lib/sddm/.config/sddm-greeter-qt5rc gid $$_sddm_g"; \
		echo "set_inode_field usr/libexec/dbus-daemon-launch-helper mode 0104755"; \
		echo "set_inode_field usr/libexec/dbus-daemon-launch-helper uid 0"; \
		echo "set_inode_field usr/libexec/dbus-daemon-launch-helper gid 86"; \
	} | debugfs -w ./part.img >/dev/null 2>&1 || true
	@echo "Installing Quake II into disk image..."
	@./scripts/populate-ext2-dir.sh ./part.img userland/quake2 opt/quake2
	@{ \
		echo "cd /"; \
		echo "mkdir usr"; \
		echo "mkdir usr/bin"; \
		echo "rm usr/bin/quake2"; \
		echo "write userland/quake2/quake2 usr/bin/quake2"; \
		echo "set_inode_field usr/bin/quake2 mode 0100755"; \
	} | debugfs -w ./part.img >/dev/null 2>&1 || true
	@echo "Installing staged glibc into /lib and /lib64 (no symlinks)..."
	@# The guest kernel does not resolve the usrmerge symlinks for the
	@# interpreter path (/lib64/ld-linux... -> usr/lib/...): with /lib64 as
	@# a symlink, exec of /bin/sh fails with
	@#   ELF load failed: File not found: /lib64/ld-linux-x86-64.so.2
	@# while a real /lib64 dir with a mismatched older glibc loads but
	@# then fails every dynamic binary with
	@#   undefined symbol: __pointer_chk_guard, version GLIBC_PRIVATE
	@# (Axon's glibc is 2.44: libc UNDs it, ld.so defines it; older glibc does not).
	@# So keep /lib and /lib64 as real dirs holding the staged runtime libs
	@# as /usr/lib. No version skew, no symlink dependency.
	@{ \
		echo "cd /"; \
		echo "rm lib"; \
		echo "rm lib64"; \
		echo "rmdir lib"; \
		echo "rmdir lib64"; \
		echo "rm_rf lib"; \
		echo "rm_rf lib64"; \
		echo "mkdir lib"; \
		echo "mkdir lib64"; \
	} | debugfs -w ./part.img >/dev/null 2>&1 || true
	@python3 scripts/image-runtime.py ./part.img build/arch/rootfs --copy-compat
	@bash scripts/image-firmware-install.sh ./part.img
	@echo "Applying AvoryOS image defaults..."
	@bash scripts/configure-image-root.sh ./part.img build/arch/rootfs
	@if [ -f userland/winoptions ] && [ -f userland/icewm-menu ]; then \
		echo "Installing IceWM configuration into disk image..."; \
		debugfs -w -R "mkdir etc/icewm" ./part.img >/dev/null 2>&1 || true; \
		debugfs -w -R "rm etc/icewm/winoptions" ./part.img >/dev/null 2>&1 || true; \
		debugfs -w -R "write userland/winoptions etc/icewm/winoptions" ./part.img >/dev/null 2>&1 || true; \
		debugfs -w -R "rm etc/icewm/menu" ./part.img >/dev/null 2>&1 || true; \
		debugfs -w -R "write userland/icewm-menu etc/icewm/menu" ./part.img >/dev/null 2>&1 || true; \
	fi
	@echo "Fixing executable modes for directly injected launchers..."
	@{ \
		echo "set_inode_field bin/avory-login mode 0100755"; \
		echo "set_inode_field bin/avory-installer mode 0100755"; \
		echo "set_inode_field usr/sbin/avory-installer mode 0100755"; \
		echo "set_inode_field bin/test_cred mode 0100755"; \
		echo "set_inode_field bin/test_accounts mode 0100755"; \
		echo "set_inode_field bin/test_hugepages mode 0100755"; \
		echo "set_inode_field bin/test_zero_page mode 0100755"; \
		echo "set_inode_field bin/test_copy_user mode 0100755"; \
		echo "set_inode_field bin/test_pcp_pmm mode 0100755"; \
		echo "set_inode_field bin/test_ticket_lock mode 0100755"; \
		echo "set_inode_field bin/test_fb_perf mode 0100755"; \
		echo "set_inode_field bin/test_vdso_bench mode 0100755"; \
		echo "set_inode_field bin/test_lazy_fpu mode 0100755"; \
		echo "set_inode_field bin/test_elf32 mode 0100755"; \
		echo "set_inode_field bin/test_heap_smp mode 0100755"; \
		echo "set_inode_field bin/test_dcache mode 0100755"; \
		echo "set_inode_field bin/test_tmpfile mode 0100755"; \
		echo "set_inode_field bin/test_kpi_dmabuf mode 0100755"; \
		echo "set_inode_field bin/test_kpi_drm mode 0100755"; \
		echo "set_inode_field bin/test_kpi_bochs mode 0100755"; \
		echo "set_inode_field bin/test_kpi_amdgpu mode 0100755"; \
		echo "set_inode_field bin/test_kpi_radeonsi mode 0100755"; \
		echo "set_inode_field bin/radeonsi_vram_leak.elf mode 0100755"; \
		echo "set_inode_field bin/test_child_notify mode 0100755"; \
		echo "set_inode_field bin/test_pty_master mode 0100755"; \
		echo "set_inode_field bin/test_uaccess_bench mode 0100755"; \
		echo "set_inode_field bin/test_mm_speed mode 0100755"; \
		echo "set_inode_field bin/test_journal_speed mode 0100755"; \
		echo "set_inode_field bin/test_readahead mode 0100755"; \
		echo "set_inode_field etc/X11/xinit/xserverrc mode 0100755"; \
		echo "set_inode_field etc/X11/xinit/xinitrc mode 0100755"; \
		echo "set_inode_field etc/xdg/xfce4/xinitrc mode 0100755"; \
		echo "set_inode_field usr/lib/dbus-daemon-launch-helper mode 0104755"; \
		echo "set_inode_field home/avory uid 1000"; \
		echo "set_inode_field home/avory gid 1000"; \
		echo "set_inode_field .config mode 040777"; \
		echo "set_inode_field .cache mode 040777"; \
	} | debugfs -w ./part.img >/dev/null 2>&1 || true

	@mkdir -p $(dir $(DISK_PARTITION_CACHE))
	@rm -f $(DISK_PARTITION_CACHE)
	@if [ -L ./part.img ]; then \
		loop=$$(readlink ./part.img); \
		rm -f ./part.img; \
		losetup -d "$$loop"; \
		python3 scripts/incremental-disk-apps.py --cache-from-disk \
			--disk disk.img --partition-cache $(DISK_PARTITION_CACHE); \
	else \
		dd if=./part.img of=disk.img bs=1M seek=1 conv=notrunc,sparse; \
		mv ./part.img $(DISK_PARTITION_CACHE); \
	fi
	@touch disk.img
	@python3 scripts/incremental-disk-apps.py --snapshot \
		--disk disk.img --partition-cache $(DISK_PARTITION_CACHE) \
		--manifest $(DISK_APP_MANIFEST) --makefile GNUmakefile \
		--apps $(USERLAND_DISK_APPS)
else
disk.img:
	@python3 scripts/incremental-disk-apps.py --update \
		--disk disk.img --partition-cache $(DISK_PARTITION_CACHE) \
		--manifest $(DISK_APP_MANIFEST) --makefile GNUmakefile \
		--apps $(USERLAND_DISK_APPS)
endif

edk2-ovmf:
	curl -L https://github.com/osdev0/edk2-ovmf-nightly/releases/latest/download/edk2-ovmf.tar.gz | gunzip | tar -xf -

limine/limine:
	rm -rf limine
	git clone https://codeberg.org/Limine/Limine.git limine --branch=v11.x-binary --depth=1
	$(MAKE) -C limine \
		CC="$(HOST_CC)" \
		CFLAGS="$(HOST_CFLAGS)" \
		CPPFLAGS="$(HOST_CPPFLAGS)" \
		LDFLAGS="$(HOST_LDFLAGS)" \
		LIBS="$(HOST_LIBS)"

.PHONY: setup
setup: $(ARCH_STAMP)
	chmod +x bootstrap.sh
	AVORY_BOOTSTRAP_SKIP_ROOTFS=1 ./bootstrap.sh

.PHONY: kernel
kernel: setup
	@if ! grep -qx 'tag=v6.18.42' kernel/linux/.pin 2>/dev/null || ! grep -qx 'commit=856a9b51680cbbed1ace1207b29e424a616cff48' kernel/linux/.pin 2>/dev/null; then scripts/linux-import.sh; fi
	@BUILD_TAG=$$(python3 scripts/kernel-build-tag.py); \
		$(MAKE) -C kernel KPI_NOUVEAU=1 BUILD_NAME='$(BUILD_NAME)' \
		BUILD_VERSION='$(BUILD_VERSION)' BUILD_TAG="$$BUILD_TAG"
	@mkdir -p build/nouveau-618
	@cp kernel/bin-x86_64/kernel build/nouveau-618/kernel
	@ln -sfn ../../kernel/linux build/nouveau-618/linux

# Optional guest kernel command line baked into the ISO (limine `cmdline:`).
# linuxkpi_param_parse() applies kpi_* module parameters from it, e.g.:
#   make run-vfio KERNEL_CMDLINE=kpi_vfio_test=1
KERNEL_CMDLINE ?= $(NOUVEAU_618_CMDLINE)

# Make tracks files, not variable values: without this stamp a new
# KERNEL_CMDLINE would silently reuse the previously baked ISO cmdline.
.PHONY: FORCE
FORCE:

build/kernel_cmdline.stamp: FORCE
	@mkdir -p build
	@printf '%s' '$(KERNEL_CMDLINE)' > $@.tmp
	@if cmp -s $@.tmp $@; then rm -f $@.tmp; else mv $@.tmp $@; fi

$(IMAGE_NAME).iso: limine/limine kernel nouveau-618-desktop-firmware limine.conf build/kernel_cmdline.stamp
	rm -rf iso_root
	mkdir -p iso_root/boot
	cp -v kernel/bin-x86_64/kernel iso_root/boot/kernel
	# Ship a stripped copy. The candidate ELF retains debug info for symbolization;
	# nothing at runtime
	# reads it, and Limine only loads the PT_LOAD segments.  Stripping the
	# copy keeps the ISO small without losing post-mortem symbolization.
	strip --strip-all iso_root/boot/kernel
	mkdir -p iso_root/boot/limine
	# The regular ISO expects disk.img as a separate QEMU drive. Only the
	# self-contained dist ISO copies the module-enabled configuration verbatim.
	sed '/^[[:space:]]*module_path: boot():\/disk.img$$/d; /^[[:space:]]*module_string: disk.img$$/d' \
		limine.conf > iso_root/boot/limine/limine.conf
	@if [ -n '$(KERNEL_CMDLINE)' ]; then \
		awk -v c='$(KERNEL_CMDLINE)' \
			'/^\// {if (target && !added) print "    cmdline: " c; target=0; if (!selected && /^\/AvoryOS/) {target=1; selected=1}} target && /^[[:space:]]*cmdline:/ {if (!added) print "    cmdline: " c; added=1; next} {print} END {if (target && !added) print "    cmdline: " c}' \
			iso_root/boot/limine/limine.conf > iso_root/boot/limine/limine.conf.tmp; \
		mv iso_root/boot/limine/limine.conf.tmp iso_root/boot/limine/limine.conf; \
	fi
	cp -v assets/boo.png iso_root/boot/limine/
	mkdir -p iso_root/EFI/BOOT
	cp -v limine/limine-bios.sys limine/limine-bios-cd.bin limine/limine-uefi-cd.bin iso_root/boot/limine/
	cp -v limine/BOOTX64.EFI iso_root/EFI/BOOT/
	cp -v limine/BOOTIA32.EFI iso_root/EFI/BOOT/
	xorriso -as mkisofs -R -r -J -b boot/limine/limine-bios-cd.bin \
		-no-emul-boot -boot-load-size 4 -boot-info-table -hfsplus \
		-apm-block-size 2048 --efi-boot boot/limine/limine-uefi-cd.bin \
		-efi-boot-part --efi-boot-image --protective-msdos-label \
		iso_root -o $(IMAGE_NAME).iso
	./limine/limine bios-install $(IMAGE_NAME).iso
	@mkdir -p build/nouveau-618
	@sha256sum $(IMAGE_NAME).iso > build/nouveau-618/avoryos-x86_64.iso.sha256
	@printf '%s\n' '$(KERNEL_CMDLINE)' > build/nouveau-618/desktop-cmdline
	rm -rf iso_root

.PHONY: clean
clean:
	$(MAKE) -C kernel clean
	rm -f $(IMAGE_NAME).iso

.PHONY: clean-all
clean-all: clean-minimal
	$(MAKE) -C kernel clean
	rm -rf iso_root $(IMAGE_NAME).iso $(IMAGE_NAME).hdd build/arch

.PHONY: clean-disk
clean-disk:
	rm -f disk.img $(DISK_PARTITION_CACHE) $(DISK_APP_MANIFEST)

.PHONY: distclean
distclean:
	$(MAKE) -C kernel distclean
	rm -rf iso_root *.iso *.hdd limine edk2-ovmf
	rm -rf userland/kria-lang

# Userland app build rules live in userland/GNUmakefile.
.PHONY: userland-build kria gpu-diagnostics test-phase6-login
userland-build: $(GLIBC_STAMP) $(ARCH_STAMP) $(GLIBC_TOOLCHAIN_STAMP)
	$(MAKE) -C userland -f GNUmakefile all

kria:
	$(MAKE) -C userland -f GNUmakefile kria.elf

gpu-diagnostics:
	$(MAKE) -C userland -f GNUmakefile gpu-diagnostics

test-phase6-login:
	./scripts/test-phase6-login.sh

# Root image recipes depend on these paths; delegate each app build to its local
# userland makefile so callers can keep using `make userland/foo.elf`.
userland/%.elf: userland/GNUmakefile config/userland-link.ld $(GLIBC_STAMP) $(GLIBC_TOOLCHAIN_STAMP)
	$(MAKE) -C userland -f GNUmakefile $*.elf

userland/test_kpi_radeonsi.elf userland/radeonsi_vram_leak.elf \
userland/test_gpu_constants.elf userland/test_gpu_pipeline.elf \
userland/test_gpu_pipeline_gl.elf userland/test_gpu_corruption.elf \
userland/test_gpu_depth.elf userland/classicube.elf:
	$(MAKE) -C userland -f GNUmakefile $(@F)

userland/terrain.png:
	$(MAKE) -C userland -f GNUmakefile terrain.png

.PHONY: all qemu clean

# Main Nouveau desktop run target: use the verified 6.18 / 570.144 ISO.
NOUVEAU_SMP ?= 8
NOUVEAU_PRESENT_BATCH ?= 1
NOUVEAU_DEBUG ?= info
NOUVEAU_DRM_DEBUG ?= 0
NOUVEAU_GPU_DIAG ?= 0
NOUVEAU_PERF ?= 1
# Prefer the 60 Hz display-info mode for the CPU-copy virtual scanout.
# High-refresh EDID modes add compositor work and reduce uncapped client FPS.
NOUVEAU_VIRTIO_EDID ?= off
NOUVEAU_618_CMDLINE := init=/sbin/init splash quiet kpi_amdgpu=1 kpi_nouveau=1 kpi_nouveau_remove_on_shutdown=1 kpi_nouveau_bdf=0000:01:00.0 nouveau.modeset=1 nouveau.runpm=0 nouveau.config=NvGspRm=1 nouveau.debug=$(NOUVEAU_DEBUG) drm.debug=$(NOUVEAU_DRM_DEBUG) virtio_gpu.present_batch=$(NOUVEAU_PRESENT_BATCH) gpu_diag=$(NOUVEAU_GPU_DIAG) gpu_perf=$(NOUVEAU_PERF)

.PHONY: nouveau-618-desktop-firmware nouveau-firmware run-nouveau
nouveau-618-desktop-firmware: kernel disk.img
	python3 scripts/nouveau-firmware-stage.py --interface 570.144 --output build/nouveau-618/firmware
	python3 scripts/image-add-firmware.py disk.img build/nouveau-618/firmware

# `make` builds the ISO and desktop firmware first. This target validates the
# ready-to-boot artifacts and host passthrough setup, then launches the normal
# installed desktop without rebuilding the ISO as a side effect.
.NOTPARALLEL: run-nouveau
run-nouveau:
	@test -f avoryos-x86_64.iso || { echo 'Run make first to build the main Nouveau ISO.' >&2; exit 1; }
	@test -f disk.img || { echo 'Run make first to build the Nouveau desktop disk.' >&2; exit 1; }
	@test ! kernel/bin-x86_64/kernel -nt avoryos-x86_64.iso || { echo 'The boot ISO predates the compiled kernel; run make before launching Nouveau.' >&2; exit 1; }
	@stale="$$(find kernel/src kernel/linuxkpi -type f -newer avoryos-x86_64.iso -print -quit)"; \
		if [ -n "$$stale" ] || [ kernel/GNUmakefile -nt avoryos-x86_64.iso ]; then \
			echo "The boot ISO predates kernel sources ($${stale:-kernel/GNUmakefile}); run make before launching Nouveau." >&2; exit 1; \
		fi
	@test -f build/nouveau-618/avoryos-x86_64.iso.sha256 && sha256sum --check --status build/nouveau-618/avoryos-x86_64.iso.sha256 || { echo 'The main AvoryOS ISO changed; run make to rebuild it.' >&2; exit 1; }
	@printf '%s\n' '$(NOUVEAU_618_CMDLINE)' | cmp -s - build/nouveau-618/desktop-cmdline || { echo 'The main ISO has different boot options; run make to rebuild it.' >&2; exit 1; }
	@test -f build/nouveau/desktop-firmware.json || { echo 'Run make to install the Nouveau firmware into the desktop disk.' >&2; exit 1; }
	@grep -Fq '"lib/firmware/nvidia/ad106/gsp/gsp-570.144.bin"' build/nouveau/desktop-firmware.json || { echo 'The desktop disk is missing the pinned 570.144 firmware; run make.' >&2; exit 1; }
	@test "$$(cat /sys/bus/pci/devices/0000:01:00.0/vendor)" = 0x10de && test "$$(cat /sys/bus/pci/devices/0000:01:00.0/device)" = 0x2803
	@test -L /sys/bus/pci/devices/0000:01:00.0/driver && test "$$(basename "$$(readlink -f /sys/bus/pci/devices/0000:01:00.0/driver)")" = vfio-pci || { echo 'RTX 4060 Ti must already be bound to vfio-pci.' >&2; exit 1; }
	@test "$$(cat /sys/bus/pci/devices/0000:01:00.1/vendor)" = 0x10de && test "$$(cat /sys/bus/pci/devices/0000:01:00.1/device)" = 0x22bd && test -L /sys/bus/pci/devices/0000:01:00.1/driver && test "$$(basename "$$(readlink -f /sys/bus/pci/devices/0000:01:00.1/driver)")" = vfio-pci || { echo 'RTX audio function must already be bound to vfio-pci.' >&2; exit 1; }
	@pin="$$(od -An -j 61 -N 1 -tu1 /sys/bus/pci/devices/0000:01:00.0/config | tr -d ' ')" && test "$$pin" -ge 1 && test "$$pin" -le 4 || { echo 'RTX PCI configuration is invalid (interrupt pin outside 1..4); stop and fully power-cycle the host before another VFIO attempt.' >&2; exit 1; }
	@test "$$(cat /sys/bus/pci/devices/0000:01:00.0/boot_vga)" = 0 || { echo 'Select the iGPU as the host display in UEFI first.' >&2; exit 1; }
	@mkdir -p build/logs
	$(MAKE) --no-print-directory run-vfio \
		RUN_ISO=avoryos-x86_64.iso RUN_ISO_DEP= RUN_DISK_DEP= \
		KERNEL_CMDLINE='$(NOUVEAU_618_CMDLINE)' \
		VFIO_BDF=0000:01:00.0 VFIO_DEVICE_EXTRA=,multifunction=on VFIO_ROM= \
		VFIO_GUEST_ADDRESS='bus=nouveau-port,addr=00.0' \
		VFIO_PREDEVICES='-device pcie-root-port,id=nouveau-port,bus=pcie.0,addr=03.0,chassis=1,hotplug=off,pref64-reserve=32G -global q35-pcihost.pci-hole64-size=64G' \
		VFIO_LAUNCHER= VFIO_SMP='$(NOUVEAU_SMP)' VFIO_POINTER=usb-mouse RUN_NIC_ADDR=08.0 \
		VFIO_VIRTIO_GPU_EXTRA=',edid=$(NOUVEAU_VIRTIO_EDID)' \
		SERIAL=chardev:nouveau-c4 \
		DISPLAY_OPT='-display gtk,zoom-to-fit=off,grab-on-hover=on,show-cursor=off,window-close=on' \
		VFIO_EXTRA='-chardev socket,id=nouveau-c4,path=build/logs/nouveau-c4-serial.sock,server=on,wait=off,logfile=build/logs/nouveau-c4.log,logappend=on $(RUN_NET_FLAGS)'

nouveau-firmware:
	@mkdir -p build/logs
	python3 scripts/nouveau-firmware-stage.py
