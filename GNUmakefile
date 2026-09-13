.SUFFIXES:

.DEFAULT_GOAL := all

ARCH := x86_64
QEMUFLAGS := -m 4G \
	-vga none \
	-device virtio-vga,xres=1280,yres=800 \
	-display gtk,zoom-to-fit=off

override IMAGE_NAME := avoryos-$(ARCH)

AVORYD_CONFIG_FILES := \
	initrd/avoryd/default.target \
	initrd/avoryd/services/system-init.service \
	initrd/avoryd/services/console.service \
	initrd/avoryd/services/wayland.service \
	initrd/avoryd/services/x11.service

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

$(QUAKE2_STAMP): $(ALPINE_STAMP) scripts/build-quake2.sh \
		scripts/quake2-sdl2-config.in scripts/quake2-avoryos-evdev.h
	./scripts/build-quake2.sh && \
		mkdir -p $(dir $(QUAKE2_STAMP)) && \
		touch $(QUAKE2_STAMP)

BUTTERSCOTCH_STAMP := build/butterscotch/.built

$(BUTTERSCOTCH_STAMP): $(ALPINE_STAMP) $(MUSL_LIBC) scripts/build-butterscotch.sh
	./scripts/build-butterscotch.sh && \
		mkdir -p $(dir $(BUTTERSCOTCH_STAMP)) && \
		touch $(BUTTERSCOTCH_STAMP)

userland/butterscotch.elf: $(BUTTERSCOTCH_STAMP)

.PHONY: butterscotch
butterscotch: $(BUTTERSCOTCH_STAMP)

.PHONY: clean-butterscotch
clean-butterscotch:
	./scripts/build-butterscotch.sh clean

HOST_CC := cc
HOST_CFLAGS := -g -O2 -pipe
HOST_CPPFLAGS :=
HOST_LDFLAGS :=
HOST_LIBS :=

# musl static sysroot (see scripts/musl-toolchain.sh). Built automatically for hello_musl / disk.img / run.
MUSL_TOOLCHAIN_BIN := $(CURDIR)/toolchain/x86_64-linux-musl/bin
MUSL_SYSROOT := $(CURDIR)/toolchain/musl-sysroot
MUSL_LIBC := $(MUSL_SYSROOT)/lib/libc.a
MUSL_CC ?= x86_64-linux-musl-gcc
MUSL_CXX ?= x86_64-linux-musl-g++
MUSL_USER_CFLAGS := -static -O2 -Wall -Wextra -fno-stack-protector \
	-I$(MUSL_SYSROOT)/include -L$(MUSL_SYSROOT)/lib
MUSL_USER_CXXFLAGS := -static -O2 -Wall -Wextra -fno-stack-protector -fno-exceptions -fno-rtti \
	-I$(MUSL_SYSROOT)/include -L$(MUSL_SYSROOT)/lib

# glibc toolchain (see scripts/glibc-toolchain.sh)
GLIBC_TOOLCHAIN_BIN := $(CURDIR)/toolchain/x86_64-linux-glibc/bin
GLIBC_SYSROOT := $(CURDIR)/toolchain/glibc-sysroot
GLIBC_CC := $(GLIBC_TOOLCHAIN_BIN)/x86_64-buildroot-linux-gnu-gcc
GLIBC_USER_CFLAGS := -O2 -Wall -Wextra -fno-stack-protector \
	--sysroot=$(GLIBC_SYSROOT)

# Alpine rootfs sysroot (built by scripts/setup-alpine.sh)
ALPINE_SYSROOT := $(CURDIR)/build/alpine/rootfs
ALPINE_STAMP := $(CURDIR)/build/alpine/.built

$(ALPINE_STAMP): scripts/setup-alpine.sh
	@mkdir -p $(dir $(ALPINE_STAMP))
	@touch $(ALPINE_STAMP)

.PHONY: setup-alpine
setup-alpine: $(ALPINE_STAMP)

GLIBC_SYSROOT := $(CURDIR)/toolchain/glibc-sysroot
GLIBC_STAMP := $(GLIBC_SYSROOT)/lib/libc.so.6

$(GLIBC_STAMP): scripts/glibc-toolchain.sh
	@echo "[*] Setting up glibc toolchain..."
	chmod +x scripts/glibc-toolchain.sh && ./scripts/glibc-toolchain.sh

.PHONY: glibc-toolchain
glibc-toolchain: $(GLIBC_STAMP)

BASH_STAMP := $(GLIBC_SYSROOT)/opt/bash/bin/bash

$(BASH_STAMP): $(GLIBC_STAMP) scripts/build-bash.sh
	@echo "[*] Building GNU Bash..."
	chmod +x scripts/build-bash.sh && ./scripts/build-bash.sh

.PHONY: bash
bash: $(BASH_STAMP)

COREUTILS_STAMP := $(GLIBC_SYSROOT)/opt/coreutils/bin/ls

$(COREUTILS_STAMP): $(GLIBC_STAMP) scripts/build-coreutils.sh
	@echo "[*] Building GNU Coreutils..."
	chmod +x scripts/build-coreutils.sh && ./scripts/build-coreutils.sh

.PHONY: coreutils
coreutils: $(COREUTILS_STAMP)


# GTK2 test - include/lib flags
GTK2_INCLUDES := \
	-I$(ALPINE_SYSROOT)/usr/include/gtk-2.0 \
	-I$(ALPINE_SYSROOT)/usr/lib/gtk-2.0/include \
	-I$(ALPINE_SYSROOT)/usr/include/glib-2.0 \
	-I$(ALPINE_SYSROOT)/usr/lib/glib-2.0/include \
	-I$(ALPINE_SYSROOT)/usr/include/pango-1.0 \
	-I$(ALPINE_SYSROOT)/usr/include/harfbuzz \
	-I$(ALPINE_SYSROOT)/usr/include/cairo \
	-I$(ALPINE_SYSROOT)/usr/include/gdk-pixbuf-2.0 \
	-I$(ALPINE_SYSROOT)/usr/include/atk-1.0 \
	-I$(ALPINE_SYSROOT)/usr/include/pixman-1 \
	-I$(ALPINE_SYSROOT)/usr/include/freetype2 \
	-I$(ALPINE_SYSROOT)/usr/include/libpng16
GTK2_LIBS := \
	-L$(ALPINE_SYSROOT)/usr/lib -L$(ALPINE_SYSROOT)/lib \
	-lgtk-x11-2.0 -lgdk-x11-2.0 -lpangocairo-1.0 -lpango-1.0 -latk-1.0 \
	-lcairo -lgdk_pixbuf-2.0 -lgio-2.0 -lgobject-2.0 -lglib-2.0 \
	-ljpeg -lmount -lblkid -leconf -lintl -lXrandr -lXinerama \
	-lgraphite2 -lXcomposite -lXdamage
GTK2_LDFLAGS := \
	-Wl,-dynamic-linker,/lib/ld-musl-x86_64.so.1 \
	-Wl,-rpath,/usr/lib \
	-Wl,-rpath-link,$(ALPINE_SYSROOT)/usr/lib

# GTK3 test - include/lib flags
GTK3_INCLUDES := \
	-I$(ALPINE_SYSROOT)/usr/include \
	-I$(ALPINE_SYSROOT)/usr/include/gtk-3.0 \
	-I$(ALPINE_SYSROOT)/usr/include/glib-2.0 \
	-I$(ALPINE_SYSROOT)/usr/lib/glib-2.0/include \
	-I$(ALPINE_SYSROOT)/usr/include/pango-1.0 \
	-I$(ALPINE_SYSROOT)/usr/include/harfbuzz \
	-I$(ALPINE_SYSROOT)/usr/include/cairo \
	-I$(ALPINE_SYSROOT)/usr/include/gdk-pixbuf-2.0 \
	-I$(ALPINE_SYSROOT)/usr/include/atk-1.0 \
	-I$(ALPINE_SYSROOT)/usr/include/pixman-1 \
	-I$(ALPINE_SYSROOT)/usr/include/freetype2 \
	-I$(ALPINE_SYSROOT)/usr/include/libpng16 \
	-I$(ALPINE_SYSROOT)/usr/include/at-spi2-atk/2.0 \
	-I$(ALPINE_SYSROOT)/usr/include/at-spi-2.0 \
	-I$(ALPINE_SYSROOT)/usr/include/dbus-1.0 \
	-I$(ALPINE_SYSROOT)/usr/lib/dbus-1.0/include \
	-I$(ALPINE_SYSROOT)/usr/include/epoxy
GTK3_LIBS := \
	-L$(ALPINE_SYSROOT)/usr/lib -L$(ALPINE_SYSROOT)/lib \
	-lgtk-3 -lgdk-3 -lpangocairo-1.0 -lpango-1.0 -latk-1.0 -latk-bridge-2.0 \
	-lcairo-gobject -lcairo -lgdk_pixbuf-2.0 -lgio-2.0 -lgobject-2.0 -lglib-2.0 \
	-lepoxy -ldbus-1 -lX11 -lXext -lXrender -lXi -lXcursor -lXfixes \
	-lwayland-client -lwayland-cursor -lwayland-egl \
	-lXrandr -lXinerama -lXcomposite -lXdamage \
	-lfontconfig -lfreetype -lpng16 -lz -lm
GTK3_LDFLAGS := \
	-Wl,-dynamic-linker,/lib/ld-musl-x86_64.so.1 \
	-Wl,-rpath,/usr/lib \
	-Wl,-rpath-link,$(ALPINE_SYSROOT)/usr/lib:$(ALPINE_SYSROOT)/lib

# Qt5 test - include/lib flags
QT5_INCLUDES := \
	-I$(ALPINE_SYSROOT)/usr/include/qt5 \
	-I$(ALPINE_SYSROOT)/usr/include/qt5/QtCore \
	-I$(ALPINE_SYSROOT)/usr/include/qt5/QtGui \
	-I$(ALPINE_SYSROOT)/usr/include/qt5/QtWidgets \
	-I$(ALPINE_SYSROOT)/usr/include/qt5/QtOpenGL \
	-I$(ALPINE_SYSROOT)/usr/include/qt5/QtPrintSupport
QT5_LIBS := \
	-L$(ALPINE_SYSROOT)/usr/lib -L$(ALPINE_SYSROOT)/lib \
	-lQt5Widgets -lQt5Gui -lQt5Core \
	-lGL -lX11 -lXext -lxcb \
	-ldl -lm -lz
QT5_CXXFLAGS := -fPIC -DQT_WIDGETS_LIB -DQT_GUI_LIB -DQT_CORE_LIB -DQT_NO_DEBUG
QT5_LDFLAGS := \
	-static-libgcc \
	-Wl,--allow-shlib-undefined \
	-Wl,-dynamic-linker,/lib/ld-musl-x86_64.so.1 \
	-Wl,-rpath,/usr/lib \
	-Wl,-rpath-link,$(ALPINE_SYSROOT)/usr/lib:$(ALPINE_SYSROOT)/lib

# SDL3 test - include/lib flags
SDL3_INCLUDES := \
	-I$(ALPINE_SYSROOT)/usr/include \
	-I$(ALPINE_SYSROOT)/usr/include/SDL3 \
	-I$(ALPINE_SYSROOT)/usr/include/SDL3_ttf
SDL3_LIBS := \
	-L$(ALPINE_SYSROOT)/usr/lib -L$(ALPINE_SYSROOT)/lib \
	-lSDL3 -lSDL3_ttf \
	-lcapstone \
	-lGL -lEGL \
	-lm \
	-lstdc++
SDL3_LDFLAGS := \
	-no-pie \
	-Wl,--allow-shlib-undefined \
	-Wl,-dynamic-linker,/lib/ld-musl-x86_64.so.1 \
	-Wl,-rpath,/usr/lib \
	-Wl,-rpath-link,$(ALPINE_SYSROOT)/usr/lib:$(ALPINE_SYSROOT)/lib

.PHONY: all
all: $(IMAGE_NAME).iso

# `make run` boots the NVMe desktop with KVM; `make run-tcg` is the same
# configuration without KVM.  The arch-specific run-x86_64 target remains the
# TCG implementation behind run-tcg.
.PHONY: run run-tcg
run: edk2-ovmf $(IMAGE_NAME).iso disk.img
	qemu-system-$(ARCH) \
		-M q35,pcspk-audiodev=snd0 \
		-drive if=pflash,unit=0,format=raw,file=edk2-ovmf/ovmf-code-$(ARCH).fd,readonly=on \
		-cdrom $(IMAGE_NAME).iso \
		-drive file=disk.img,format=raw,if=none,id=nvme0 \
		-device nvme,serial=avoryos0,drive=nvme0 \
		-cpu host -enable-kvm \
		-smp 4 \
		-serial stdio \
		-audiodev pa,id=snd0,timer-period=2000,out.frequency=48000,out.channels=2,out.format=s16,out.buffer-length=500000,out.latency=500000 \
		-device rtl8139,netdev=net0 \
		-netdev user,id=net0 \
		-device intel-hda -device hda-duplex,audiodev=snd0 \
		-device qemu-xhci,id=xhci \
		-device usb-kbd,bus=xhci.0 \
		-device usb-mouse,bus=xhci.0 \
		$(QEMUFLAGS)
run-tcg: run-x86_64

# ── LinuxKPI DRM canary (Phase 4+) ──────────────────────────────────────────
#
# `make run` plus the emulated devices the LinuxKPI PCI/DRM chunks validate
# against: `edu` (1234:11e8, Phase 4 C4) and `bochs-display` (1234:1111, the
# TTM canary, Phase 4 C5).  bochs-display is a secondary, non-VGA display, so
# it coexists with virtio-vga and does not touch the native GOP path.
# `make run` is deliberately untouched (the PCI suite logs SKIP there).
# Headless evidence capture:
#
#   make run-linuxdrm SERIAL=file:build/logs/p4-c4.log DISPLAY_OPT=-display none
#
# (override QEMU_MEM=... for smaller machines).
SERIAL ?= stdio
DISPLAY_OPT ?= -display gtk,zoom-to-fit=off
QEMU_MEM ?= -m 4G

.PHONY: run-linuxdrm
run-linuxdrm: edk2-ovmf $(IMAGE_NAME).iso disk.img
	qemu-system-$(ARCH) \
		-M q35,pcspk-audiodev=snd0 \
		-drive if=pflash,unit=0,format=raw,file=edk2-ovmf/ovmf-code-$(ARCH).fd,readonly=on \
		-cdrom $(IMAGE_NAME).iso \
		-drive file=disk.img,format=raw,if=none,id=nvme0 \
		-device nvme,serial=avoryos0,drive=nvme0 \
		-cpu host -enable-kvm \
		-smp 4 \
		-serial $(SERIAL) \
		$(QEMU_MEM) \
		-vga none \
		-device virtio-vga,xres=1280,yres=800 \
		-device bochs-display \
		$(DISPLAY_OPT) \
		-device edu \
		-audiodev pa,id=snd0,timer-period=2000,out.frequency=48000,out.channels=2,out.format=s16,out.buffer-length=500000,out.latency=500000 \
		-device rtl8139,netdev=net0 \
		-netdev user,id=net0 \
		-device intel-hda -device hda-duplex,audiodev=snd0 \
		-device qemu-xhci,id=xhci \
		-device usb-kbd,bus=xhci.0 \
		-device usb-mouse,bus=xhci.0

# ── VFIO passthrough (Phase 0+) ──────────────────────────────────────────────
#
# Boots AvoryOS with a host GPU passed through to QEMU.  The guest sees the
# real device; once the imported Linux amdgpu driver exists, it binds to it.
# Display output goes to the passed-through GPU's physical connectors, so this
# target is headless from QEMU's point of view and the serial line is the
# console.  An emulated std VGA remains so OVMF/Limine still get a GOP
# framebuffer for the native kernel console.
#
#   scripts/vfio-vbios.sh 0000:0e:00.0    # host: extract VBIOS into build/vfio/
#   make run-vfio
#
# Override with VFIO_BDF=... VFIO_MEM=... VFIO_ROM=... QEMUFLAGS='...'.
VFIO_BDF ?= 0000:0e:00.0
VFIO_MEM ?= 4G
VFIO_ROM ?= build/vfio/vbios.rom
VFIO_COMMA := ,
VFIO_ROM_OPT = $(if $(wildcard $(VFIO_ROM)),$(VFIO_COMMA)romfile=$(VFIO_ROM),)

.PHONY: run-vfio
run-vfio: edk2-ovmf $(IMAGE_NAME).iso disk.img
	qemu-system-$(ARCH) \
		-M q35 \
		-m $(VFIO_MEM) \
		-cpu host -enable-kvm \
		-smp 4 \
		-drive if=pflash,unit=0,format=raw,file=edk2-ovmf/ovmf-code-$(ARCH).fd,readonly=on \
		-cdrom $(IMAGE_NAME).iso \
		-drive file=disk.img,format=raw,if=none,id=nvme0 \
		-device nvme,serial=avoryos0,drive=nvme0 \
		-device vfio-pci,host=$(VFIO_BDF),rombar=1$(VFIO_ROM_OPT) \
		-display none \
		-serial stdio \
		$(QEMUFLAGS)

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

.PHONY: run-x86_64
# Default interactive target on a machine without /dev/kvm: TCG, no KVM flag,
# root filesystem on an NVMe namespace.  run-sata-tcg keeps the old IDE/ATA
# layout for comparison.  Use QEMUFLAGS='-smp N' for SMP tests.
run-x86_64: edk2-ovmf $(IMAGE_NAME).iso disk.img
	qemu-system-$(ARCH) \
		-M q35,pcspk-audiodev=snd0 \
		-drive if=pflash,unit=0,format=raw,file=edk2-ovmf/ovmf-code-$(ARCH).fd,readonly=on \
		-cdrom $(IMAGE_NAME).iso \
		-drive file=disk.img,format=raw,if=none,id=nvme0 \
		-device nvme,serial=avoryos0,drive=nvme0 \
		-smp 4 \
		-serial stdio \
		-audiodev pa,id=snd0,timer-period=2000,out.frequency=48000,out.channels=2,out.format=s16,out.buffer-length=500000,out.latency=500000 \
		-device rtl8139,netdev=net0 \
		-netdev user,id=net0 \
		-device intel-hda -device hda-duplex,audiodev=snd0 \
		-device qemu-xhci,id=xhci \
		-device usb-kbd,bus=xhci.0 \
		-device usb-mouse,bus=xhci.0 \
		$(QEMUFLAGS)


# Legacy SATA/IDE root disk (the pre-NVMe run layout), kept for AHCI/ATA
# regression and A/B comparisons.  run-sata is KVM, run-sata-tcg is TCG.
.PHONY: run-sata run-sata-tcg
run-sata: edk2-ovmf $(IMAGE_NAME).iso disk.img
	qemu-system-$(ARCH) \
		-M q35,pcspk-audiodev=snd0 \
		-drive if=pflash,unit=0,format=raw,file=edk2-ovmf/ovmf-code-$(ARCH).fd,readonly=on \
		-cdrom $(IMAGE_NAME).iso \
		-drive file=disk.img,format=raw,if=none,id=disk0 -device ide-hd,drive=disk0,bus=ide.0 \
		-cpu host -enable-kvm \
		-smp 4 \
		-serial stdio \
		-audiodev pa,id=snd0,timer-period=2000,out.frequency=48000,out.channels=2,out.format=s16,out.buffer-length=500000,out.latency=500000 \
		-device rtl8139,netdev=net0 \
		-netdev user,id=net0 \
		-device intel-hda -device hda-duplex,audiodev=snd0 \
		-device qemu-xhci,id=xhci \
		-device usb-kbd,bus=xhci.0 \
		-device usb-mouse,bus=xhci.0 \
		$(QEMUFLAGS)

run-sata-tcg: edk2-ovmf $(IMAGE_NAME).iso disk.img
	qemu-system-$(ARCH) \
		-M q35,pcspk-audiodev=snd0 \
		-drive if=pflash,unit=0,format=raw,file=edk2-ovmf/ovmf-code-$(ARCH).fd,readonly=on \
		-cdrom $(IMAGE_NAME).iso \
		-drive file=disk.img,format=raw,if=none,id=disk0 -device ide-hd,drive=disk0,bus=ide.0 \
		-smp 4 \
		-serial stdio \
		-audiodev pa,id=snd0,timer-period=2000,out.frequency=48000,out.channels=2,out.format=s16,out.buffer-length=500000,out.latency=500000 \
		-device rtl8139,netdev=net0 \
		-netdev user,id=net0 \
		-device intel-hda -device hda-duplex,audiodev=snd0 \
		-device qemu-xhci,id=xhci \
		-device usb-kbd,bus=xhci.0 \
		-device usb-mouse,bus=xhci.0 \
		$(QEMUFLAGS)

		

# ── NVMe ────────────────────────────────────────────────────────────────────

# run-nvme / run-nvme-tcg predate the switch of the interactive targets to
# NVMe; they are aliases now.
.PHONY: run-nvme run-nvme-tcg
run-nvme: run
run-nvme-tcg: run-tcg

# Headless test image: minimal root on AHCI plus a blank NVMe scratch disk.
# The image runs /bin/nvme_test auto at boot and powers off; the host-side
# orchestrator is scripts/nvme/nvme-stress.sh.
.PHONY: run-nvme-test
run-nvme-test: edk2-ovmf $(IMAGE_NAME).iso nvme_test.img nvme_scratch.img
	@mkdir -p build/nvme
	qemu-system-$(ARCH) \
		-M q35 \
		-m 4G \
		-drive if=pflash,unit=0,format=raw,file=edk2-ovmf/ovmf-code-$(ARCH).fd,readonly=on \
		-cdrom $(IMAGE_NAME).iso \
		-drive file=nvme_test.img,format=raw,if=none,id=root0 \
		-device ide-hd,drive=root0,bus=ide.0 \
		-drive file=nvme_scratch.img,format=raw,if=none,id=nvme0 \
		-device nvme,serial=avoryos-test,drive=nvme0 \
		$(if $(wildcard /dev/kvm),-cpu host -enable-kvm) \
		-smp 4 \
		-display none \
		-serial file:build/nvme/run-nvme-test.log

nvme_test.img: scripts/nvme/create-nvme-test.sh userland/nvme_test.elf userland/nvme_bench.elf userland/avoryd.elf userland/shutdown.elf
	./scripts/nvme/create-nvme-test.sh

nvme_scratch.img: nvme_test.img
	@test -f $@ || ./scripts/nvme/create-nvme-test.sh

# Phase 7 acceptance & hardening suites (scripts/nvme/nvme-phase7.sh).  These build
# and boot QEMU many times; PHASE7_ARGS forwards extra runner flags, e.g.
#   make nvme-phase7 PHASE7_ARGS="--quick"
#   make nvme-phase7-matrix PHASE7_ARGS="--only=accel=tcg --limit=8"
#   make nvme-phase7-dry            # prints the 72-cell matrix and commands
.PHONY: nvme-phase7 nvme-phase7-quick nvme-phase7-dry nvme-phase7-matrix
nvme-phase7:
	./scripts/nvme/nvme-phase7.sh all $(PHASE7_ARGS)
nvme-phase7-quick:
	./scripts/nvme/nvme-phase7.sh --quick all $(PHASE7_ARGS)
nvme-phase7-dry:
	./scripts/nvme/nvme-phase7.sh --dry-run all $(PHASE7_ARGS)
nvme-phase7-matrix:
	./scripts/nvme/nvme-phase7.sh matrix $(PHASE7_ARGS)

.PHONY: run-net
# Headless KVM boot for network benchmarks.  The guest reaches host-side
# test servers (scripts/nettest-server.py) at 10.0.2.2 under user networking.
run-net: edk2-ovmf $(IMAGE_NAME).iso disk.img
	qemu-system-$(ARCH) \
		-M q35 \
		-m 4G \
		-drive if=pflash,unit=0,format=raw,file=edk2-ovmf/ovmf-code-$(ARCH).fd,readonly=on \
		-cdrom $(IMAGE_NAME).iso \
		-drive file=disk.img,format=raw,if=none,id=disk0 -device ide-hd,drive=disk0,bus=ide.0 \
		-cpu host -enable-kvm \
		-smp 4 \
		-serial mon:stdio \
		-display none \
		-device rtl8139,netdev=net0 \
		-netdev user,id=net0

.PHONY: run-bios
run-bios: $(IMAGE_NAME).iso disk.img
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
run-ata: edk2-ovmf $(IMAGE_NAME).iso disk.img
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
		-device usb-mouse,bus=xhci.0 \
		$(QEMUFLAGS)

AetherDE/x11-wm/AetherWM: AetherDE/x11-wm/main.c AetherDE/x11-wm/render.c AetherDE/x11-wm/input.c AetherDE/x11-wm/frames.c AetherDE/x11-wm/wm.h AetherDE/x11-wm/stb_image.h $(ALPINE_STAMP) $(MUSL_LIBC)
	@echo "[*] Compiling AetherDE X11 Window Manager (low-level: Xft+XRender, no Cairo/Pango)..."
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" \
		$(MUSL_CC) -O2 -Wall -Wextra -march=x86-64 -mtune=generic \
		-isystem $(ALPINE_SYSROOT)/usr/include \
		-isystem $(ALPINE_SYSROOT)/usr/include/freetype2 \
		-IAetherDE/x11-wm \
		AetherDE/x11-wm/main.c \
		AetherDE/x11-wm/render.c \
		AetherDE/x11-wm/input.c \
		AetherDE/x11-wm/frames.c \
		-o AetherDE/x11-wm/AetherWM \
		-L$(ALPINE_SYSROOT)/usr/lib -L$(ALPINE_SYSROOT)/lib \
		-lX11 \
		-lXrender \
		-lXft \
		-lXext \
		-lfontconfig -lfreetype -lm \
		-Wl,-dynamic-linker,/lib/ld-musl-x86_64.so.1 \
		-Wl,-rpath,/usr/lib \
		-Wl,-rpath-link,$(ALPINE_SYSROOT)/usr/lib:$(ALPINE_SYSROOT)/lib


AetherDE/aether-dock/aether-dock: AetherDE/aether-dock/main.c AetherDE/aether-dock/dock.c AetherDE/aether-dock/render_dock.c AetherDE/aether-dock/dock.h $(ALPINE_STAMP) $(MUSL_LIBC)
	@echo "[*] Compiling AetherDE Dock..."
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" \
		$(MUSL_CC) -O2 -Wall -Wextra -march=x86-64 -mtune=generic \
		-I$(ALPINE_SYSROOT)/usr/include \
		-I$(ALPINE_SYSROOT)/usr/include/gtk-3.0 \
		-I$(ALPINE_SYSROOT)/usr/include/gdk-pixbuf-2.0 \
		-I$(ALPINE_SYSROOT)/usr/include/atk-1.0 \
		-I$(ALPINE_SYSROOT)/usr/include/at-spi2-atk/2.0 \
		-I$(ALPINE_SYSROOT)/usr/include/at-spi-2.0 \
		-I$(ALPINE_SYSROOT)/usr/include/dbus-1.0 \
		-I$(ALPINE_SYSROOT)/usr/lib/dbus-1.0/include \
		-I$(ALPINE_SYSROOT)/usr/include/epoxy \
		-I$(ALPINE_SYSROOT)/usr/include/cairo \
		-I$(ALPINE_SYSROOT)/usr/include/pango-1.0 \
		-I$(ALPINE_SYSROOT)/usr/include/glib-2.0 \
		-I$(ALPINE_SYSROOT)/usr/lib/glib-2.0/include \
		-I$(ALPINE_SYSROOT)/usr/include/harfbuzz \
		-I$(ALPINE_SYSROOT)/usr/include/pixman-1 \
		-I$(ALPINE_SYSROOT)/usr/include/freetype2 \
		-I$(ALPINE_SYSROOT)/usr/include/libpng16 \
		AetherDE/aether-dock/main.c \
		AetherDE/aether-dock/dock.c \
		AetherDE/aether-dock/render_dock.c \
		-o AetherDE/aether-dock/aether-dock \
		-L$(ALPINE_SYSROOT)/usr/lib -L$(ALPINE_SYSROOT)/lib \
		-lgtk-3 -lgdk-3 \
		-lgdk_pixbuf-2.0 \
		-latk-1.0 \
		-lcairo-gobject -lcairo \
		-lepoxy \
		-lpangocairo-1.0 -lpango-1.0 \
		-lgio-2.0 -lgobject-2.0 -lglib-2.0 \
		-lX11 -lXext -lXrender -lXi -lXcursor -lXfixes \
		-lXrandr -lXinerama -lXcomposite -lXdamage \
		-lfontconfig -lfreetype -lpng16 -lz -lm \
		-Wl,-dynamic-linker,/lib/ld-musl-x86_64.so.1 \
		-Wl,-rpath,/usr/lib \
		-Wl,-rpath-link,$(ALPINE_SYSROOT)/usr/lib:$(ALPINE_SYSROOT)/lib


AetherDE/aether-panel/aether-panel: AetherDE/aether-panel/panel.c $(ALPINE_STAMP) $(MUSL_LIBC)
	@echo "[*] Compiling AetherDE Panel..."
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" \
		$(MUSL_CC) -O2 -Wall -Wextra \
		-Wno-format-truncation -Wno-stringop-truncation \
		-march=x86-64 -mtune=generic \
		-I$(ALPINE_SYSROOT)/usr/include \
		-I$(ALPINE_SYSROOT)/usr/include/cairo \
		-I$(ALPINE_SYSROOT)/usr/include/pango-1.0 \
		-I$(ALPINE_SYSROOT)/usr/include/glib-2.0 \
		-I$(ALPINE_SYSROOT)/usr/lib/glib-2.0/include \
		-I$(ALPINE_SYSROOT)/usr/include/harfbuzz \
		-I$(ALPINE_SYSROOT)/usr/include/pixman-1 \
		-I$(ALPINE_SYSROOT)/usr/include/freetype2 \
		-I$(ALPINE_SYSROOT)/usr/include/libpng16 \
		AetherDE/aether-panel/panel.c \
		-o AetherDE/aether-panel/aether-panel \
		-L$(ALPINE_SYSROOT)/usr/lib -L$(ALPINE_SYSROOT)/lib \
		-lcairo \
		-lpangocairo-1.0 -lpango-1.0 \
		-lgobject-2.0 -lglib-2.0 \
		-lX11 \
		-lfontconfig -lfreetype -lpng16 -lz -lm \
		-Wl,-dynamic-linker,/lib/ld-musl-x86_64.so.1 \
		-Wl,-rpath,/usr/lib \
		-Wl,-rpath-link,$(ALPINE_SYSROOT)/usr/lib:$(ALPINE_SYSROOT)/lib


AetherDE/wayland-compositor/aether-compositor: AetherDE/wayland-compositor/main.c AetherDE/wayland-compositor/render.c AetherDE/wayland-compositor/input.c AetherDE/wayland-compositor/pixel_server.c AetherDE/wayland-compositor/protocols.c AetherDE/wayland-compositor/xdg-shell-protocol.c $(ALPINE_STAMP) $(MUSL_LIBC)
	@echo "[*] Compiling AetherDE/wayland-compositor modular source files..."
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" \
		$(MUSL_CC) -O2 -Wall -march=x86-64 -mtune=generic \
		-I$(ALPINE_SYSROOT)/usr/include \
		-I$(ALPINE_SYSROOT)/usr/include/cairo \
		-I$(ALPINE_SYSROOT)/usr/include/pixman-1 \
		AetherDE/wayland-compositor/main.c \
		AetherDE/wayland-compositor/render.c \
		AetherDE/wayland-compositor/input.c \
		AetherDE/wayland-compositor/pixel_server.c \
		AetherDE/wayland-compositor/protocols.c \
		AetherDE/wayland-compositor/xdg-shell-protocol.c \
		-L$(ALPINE_SYSROOT)/usr/lib \
		-Wl,-dynamic-linker,/lib/ld-musl-x86_64.so.1 \
		-Wl,-rpath,/usr/lib \
		-Wl,-rpath-link,$(ALPINE_SYSROOT)/usr/lib \
		-lwayland-server -lcairo -lpixman-1 -lfontconfig -lfreetype -lpng16 -lz -lm \
		-o AetherDE/wayland-compositor/aether-compositor


AetherDE/demo-client/aether-window: AetherDE/demo-client/main.c $(ALPINE_STAMP) $(MUSL_LIBC)
	@echo "[*] Compiling AetherDE GTK3 Wayland Window Client..."
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" \
		$(MUSL_CC) -O2 -Wall -march=x86-64 -mtune=generic \
		AetherDE/demo-client/main.c \
		-o AetherDE/demo-client/aether-window \
		$(GTK3_INCLUDES) \
		$(GTK3_LIBS) \
		$(GTK3_LDFLAGS)


# Create a 5GB ext4 disk image with sample files for testing
# Deterministic 4 KB firmware blob read by the Phase 5 firmware self-test
# (byte i = (i * 7 + 3) & 0xff).  Staged into the rootfs /lib/firmware below.
build/test_fw.bin: GNUmakefile
	@mkdir -p build
	@python3 -c 'import sys; open(sys.argv[1], "wb").write(bytes(((i * 7 + 3) & 0xff) for i in range(4096)))' $@

disk.img: GNUmakefile userland/winoptions userland/icewm-menu $(ALPINE_STAMP)
disk.img: scripts/configure-accounts.sh userland/avory-account userland/test_accounts.sh userland/avory-login.elf
disk.img:  userland/dns_lookup.elf userland/nettest.elf
disk.img: userland/test_clone_futex.elf
disk.img: userland/test_futex_pi.elf
disk.img: userland/test_unix_sockets.elf
disk.img: userland/test_syscall_speed.elf
disk.img: userland/test_hugepages.elf
disk.img: userland/test_zero_page.elf
disk.img: userland/test_copy_user.elf
disk.img: userland/test_pcp_pmm.elf
disk.img: userland/test_ticket_lock.elf
disk.img: userland/test_fb_perf.elf
disk.img: userland/test_vdso_bench.elf
disk.img: userland/test_lazy_fpu.elf
disk.img: userland/test_heap_smp.elf
disk.img: userland/test_dcache.elf
disk.img: userland/test_tmpfile.elf
disk.img: userland/test_child_notify.elf
disk.img: userland/test_pty_master.elf
disk.img: userland/test_uaccess_bench.elf
disk.img: userland/proc_bench.elf
disk.img: userland/test_watchdog.elf
disk.img: userland/test_kpi_dmabuf.elf
disk.img: userland/test_kpi_drm.elf
disk.img: userland/test_kpi_bochs.elf
disk.img: build/test_fw.bin
disk.img: userland/butterscotch.elf assets/game.unx assets/assets


disk.img: $(BASH_STAMP) $(COREUTILS_STAMP) $(ALPINE_STAMP) $(QUAKE2_BUNDLE_FILES)
disk.img: assets/boot.wav userland/test.c assets/test.wav assets/jane.mp3 assets/mc9.mp3 assets/train.mp3 assets/test.bmp assets/test.tar assets/room.png assets/logo.png assets/linus.gif assets/video.mp4 userland/forkit.elf userland/about.elf userland/hello_glibc.elf userland/booter.elf userland/reboot.elf userland/shutdown.elf userland/apm.elf userland/test_cpp.elf  userland/kilo.elf  userland/ls.elf userland/lspci.elf userland/lsblk.elf userland/readelf.elf userland/pong.elf userland/raycast.elf userland/asplay.elf userland/kria.elf userland/doom.elf userland/doom_x11.elf userland/gtk_test.elf userland/qt5_test.elf userland/sdl3_test.elf userland/tglgears_fb.elf userland/tglgears_drm.elf userland/tglhello_drm.elf userland/test_mem_stress.elf userland/classicube.elf userland/terrain.png userland/texpacks/classicube.zip initrd/startx.sh initrd/startw.sh initrd/weston.ini AetherDE/x11-wm/AetherWM AetherDE/aether-dock/aether-dock AetherDE/aether-panel/aether-panel AetherDE/wayland-compositor/aether-compositor AetherDE/demo-client/aether-window AetherDE/scripts/sax11.sh AetherDE/scripts/sawayland.sh userland/avoryd.elf $(AVORYD_CONFIG_FILES)
	@echo "Creating root filesystem (ext4)..."
	rm -f ./part.img
	dd if=/dev/zero of=./part.img bs=1M count=5119
	mkfs.ext4 -F -b 1024 -I 128 \
		-O extent,filetype,has_journal,dir_index,^64bit,^metadata_csum,^flex_bg,^huge_file,^dir_nlink,^extra_isize,^metadata_csum_seed,^orphan_file \
		./part.img
	@echo "Populating root filesystem..."
	@{ \
		echo "cd /"; \
		echo "mkdir tmp"; \
		echo "mkdir bin"; \
		echo "mkdir lib"; \
		echo "rm bin/startx.sh"; \
		echo "write initrd/startx.sh bin/startx.sh"; \
		echo "rm bin/startw.sh"; \
		echo "write initrd/startw.sh bin/startw.sh"; \
		echo "rm bin/sax11.sh"; \
		echo "write AetherDE/scripts/sax11.sh bin/sax11.sh"; \
		echo "rm bin/sawayland.sh"; \
		echo "write AetherDE/scripts/sawayland.sh bin/sawayland.sh"; \
		echo "rm bin/AetherWM"; \
		echo "write AetherDE/x11-wm/AetherWM bin/AetherWM"; \
		echo "rm bin/aether-dock"; \
		echo "write AetherDE/aether-dock/aether-dock bin/aether-dock"; \
		echo "rm bin/aether-panel"; \
		echo "write AetherDE/aether-panel/aether-panel bin/aether-panel"; \
		echo "rm bin/aether-compositor"; \
		echo "write AetherDE/wayland-compositor/aether-compositor bin/aether-compositor"; \
		echo "rm bin/aether-window"; \
		echo "write AetherDE/demo-client/aether-window bin/aether-window"; \
		echo "rm bin/avoryd"; \
		echo "write userland/avoryd.elf bin/avoryd"; \
		echo "rm bin/avory-login"; \
		echo "write userland/avory-login.elf bin/avory-login"; \
		echo "mkdir etc"; \
		echo "mkdir etc/apm"; \
		echo "mkdir etc/apm/cache"; \
		echo "mkdir etc/apm/installed"; \
		echo "mkdir etc/avoryd"; \
		echo "mkdir etc/avoryd/services"; \
		echo "rm etc/avoryd/default.target"; \
		echo "write initrd/avoryd/default.target etc/avoryd/default.target"; \
		echo "rm etc/avoryd/services/system-init.service"; \
		echo "write initrd/avoryd/services/system-init.service etc/avoryd/services/system-init.service"; \
		echo "rm etc/avoryd/services/console.service"; \
		echo "write initrd/avoryd/services/console.service etc/avoryd/services/console.service"; \
		echo "rm etc/avoryd/services/wayland.service"; \
		echo "write initrd/avoryd/services/wayland.service etc/avoryd/services/wayland.service"; \
		echo "rm etc/avoryd/services/x11.service"; \
		echo "write initrd/avoryd/services/x11.service etc/avoryd/services/x11.service"; \
		echo "rm etc/weston.ini"; \
		echo "write initrd/weston.ini etc/weston.ini"; \
		echo "rm lib/libc.so"; \
		echo "write toolchain/musl-sysroot/lib/libc.so lib/libc.so"; \
		echo "rm lib/ld-musl-x86_64.so.1"; \
		echo "write toolchain/musl-sysroot/lib/libc.so lib/ld-musl-x86_64.so.1"; \
		echo "mkdir lib64"; \
		echo "rm lib64/libc.so.6"; \
		echo "write toolchain/glibc-sysroot/lib/libc.so.6 lib64/libc.so.6"; \
		echo "rm lib64/libm.so.6"; \
		echo "write toolchain/glibc-sysroot/lib/libm.so.6 lib64/libm.so.6"; \
		echo "rm lib64/ld-linux-x86-64.so.2"; \
		echo "write toolchain/glibc-sysroot/lib/ld-linux-x86-64.so.2 lib64/ld-linux-x86-64.so.2"; \
		echo "rm lib64/libresolv.so.2"; \
		echo "write toolchain/glibc-sysroot/lib/libresolv.so.2 lib64/libresolv.so.2"; \
	} | debugfs -w ./part.img >/dev/null 2>&1 || true
	@if [ -d AetherDE ]; then \
		echo "Installing AetherDE into disk image (/AetherDE)..."; \
		./scripts/populate-ext2-dir.sh ./part.img AetherDE AetherDE; \
	fi
	@if [ -d toolchain/glibc-sysroot/usr/include ]; then \
		echo "Installing GLIBC headers into disk image (opt/glibc/include)..."; \
		./scripts/populate-ext2-dir.sh ./part.img toolchain/glibc-sysroot/usr/include opt/glibc/include; \
	fi
	@if [ -d toolchain/glibc-sysroot/usr/lib ]; then \
		echo "Installing GLIBC libs into /usr/lib64..."; \
		./scripts/populate-ext2-dir.sh ./part.img toolchain/glibc-sysroot/usr/lib usr/lib64; \
	fi
	@{ \
		echo "cd /"; \
		echo "rm bin/netlink_test"; \
		echo "write userland/netlink_test.elf bin/netlink_test"; \
		echo "rm bin/kilo"; \
		echo "write userland/kilo.elf bin/kilo"; \
		echo "rm bin/about"; \
		echo "write userland/about.elf bin/about"; \
		echo "rm bin/hello_glibc"; \
		echo "write userland/hello_glibc.elf bin/hello_glibc"; \
		echo "rm bin/test_cpp"; \
		echo "write userland/test_cpp.elf bin/test_cpp"; \
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
		echo "rm bin/apm"; \
		echo "write userland/apm.elf bin/apm"; \
		echo "rm bin/test_cred"; \
		echo "write userland/test_cred.elf bin/test_cred"; \
		echo "rm test.s"; \
		echo "write userland/test.s test.s"; \
		echo "rm standalone.s"; \
		echo "write userland/standalone.s standalone.s"; \
		echo "rm test.wav"; \
		echo "write assets/test.wav test.wav"; \
		echo "rm boot.wav"; \
		echo "write assets/boot.wav boot.wav"; \
		echo "rm test.c"; \
		echo "write userland/test.c test.c"; \
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
		echo "rm test.krx"; \
		echo "write userland/kria-lang/test.krx test.krx"; \
		echo "rm hello.krx"; \
		echo "write userland/hello.krx hello.krx"; \
		echo "rm bin/doom"; \
		echo "write userland/doom.elf bin/doom"; \
		echo "rm bin/dns_lookup"; \
		echo "write userland/dns_lookup.elf bin/dns_lookup"; \
		echo "write userland/nettest.elf bin/nettest"; \
		echo "rm test.tar"; \
		echo "write assets/test.tar test.tar"; \
		echo "rm bin/tglgears"; \
		echo "write userland/tglgears_fb.elf bin/tglgears"; \
		echo "rm bin/tglgears_drm"; \
		echo "write userland/tglgears_drm.elf bin/tglgears_drm"; \
		echo "rm bin/tglhello_drm"; \
		echo "write userland/tglhello_drm.elf bin/tglhello_drm"; \
		echo "rm bin/test_mem_stress"; \
		echo "write userland/test_mem_stress.elf bin/test_mem_stress"; \
		echo "rm bin/test_clone_futex"; \
		echo "write userland/test_clone_futex.elf bin/test_clone_futex"; \
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
		echo "rm bin/test_child_notify"; \
		echo "write userland/test_child_notify.elf bin/test_child_notify"; \
		echo "rm bin/test_pty_master"; \
		echo "write userland/test_pty_master.elf bin/test_pty_master"; \
		echo "rm bin/test_syscall_speed"; \
		echo "write userland/test_syscall_speed.elf bin/test_syscall_speed"; \
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
		echo "rm bin/forkit.elf"; \
		echo "write userland/forkit.elf bin/forkit.elf"; \
		echo "rm bin/fault_mon"; \
		echo "write userland/fault_mon.elf bin/fault_mon"; \
	} | debugfs -w ./part.img >/dev/null 2>&1 || true






	@echo "Writing ClassiCube options.txt (texture pack config)..."
	@printf 'texture-pack=classicube.zip\nskin-server=\n' > /tmp/classicube_options.txt
	debugfs -w -R "rm options.txt" ./part.img >/dev/null 2>&1 || true
	debugfs -w -R "write /tmp/classicube_options.txt options.txt" ./part.img >/dev/null 2>&1 || true
	rm -f /tmp/classicube_options.txt
	rm -f /tmp/avoryos_hello.txt /tmp/avoryos_readme.txt
	@echo "Installing Forkit assets (fonts + test pages) into disk image..."
	@{ \
		echo "cd /"; \
		echo "mkdir usr"; \
		echo "mkdir usr/share"; \
		echo "mkdir usr/share/forkit"; \
		echo "mkdir usr/share/forkit/assets"; \
		echo "mkdir usr/share/forkit/assets/fonts"; \
		echo "rm usr/share/forkit/assets/test.html"; \
		echo "write build/forkit/assets/test.html usr/share/forkit/assets/test.html"; \
		echo "rm usr/share/forkit/assets/html-test.html"; \
		echo "write build/forkit/assets/html-test.html usr/share/forkit/assets/html-test.html"; \
		echo "rm usr/share/forkit/assets/css-test.html"; \
		echo "write build/forkit/assets/css-test.html usr/share/forkit/assets/css-test.html"; \
		echo "rm usr/share/forkit/assets/js-test.html"; \
		echo "write build/forkit/assets/js-test.html usr/share/forkit/assets/js-test.html"; \
		echo "rm usr/share/forkit/assets/fonts/NotoSans-Regular.ttf"; \
		echo "write build/forkit/assets/fonts/NotoSans-Regular.ttf usr/share/forkit/assets/fonts/NotoSans-Regular.ttf"; \
		echo "rm usr/share/forkit/assets/fonts/NotoSans-Bold.ttf"; \
		echo "write build/forkit/assets/fonts/NotoSans-Bold.ttf usr/share/forkit/assets/fonts/NotoSans-Bold.ttf"; \
		echo "rm usr/share/forkit/assets/fonts/NotoSans-Italic.ttf"; \
		echo "write build/forkit/assets/fonts/NotoSans-Italic.ttf usr/share/forkit/assets/fonts/NotoSans-Italic.ttf"; \
		echo "rm usr/share/forkit/assets/fonts/NotoSans-BoldItalic.ttf"; \
		echo "write build/forkit/assets/fonts/NotoSans-BoldItalic.ttf usr/share/forkit/assets/fonts/NotoSans-BoldItalic.ttf"; \
		echo "rm usr/share/forkit/assets/fonts/NotoSansMono-Regular.ttf"; \
		echo "write build/forkit/assets/fonts/NotoSansMono-Regular.ttf usr/share/forkit/assets/fonts/NotoSansMono-Regular.ttf"; \
		echo "rm usr/share/forkit/assets/fonts/NotoSansMono-Bold.ttf"; \
		echo "write build/forkit/assets/fonts/NotoSansMono-Bold.ttf usr/share/forkit/assets/fonts/NotoSansMono-Bold.ttf"; \
		echo "rm bin/forkit"; \
		echo "write userland/forkit.elf bin/forkit"; \
	} | debugfs -w ./part.img >/dev/null 2>&1 || true
	@if [ -d build/alpine/rootfs ]; then \
		mkdir -p build/alpine/rootfs/lib/firmware; \
		cp -f build/test_fw.bin build/alpine/rootfs/lib/firmware/test_fw.bin; \
	fi
	@if [ -d build/alpine/rootfs ]; then \
		echo "Populating Alpine Linux rootfs into disk image..."; \
		./scripts/configure-accounts.sh build/alpine/rootfs; \
		./scripts/populate-ext2-dir.sh ./part.img build/alpine/rootfs /; \
	fi
	# The Alpine rootfs also has a stale bin/startx.sh.  Restore these
	# AvoryOS-owned files after the overlay, otherwise XFCE runs startxfce4
	# and opens its failsafe-session error dialog.
	@{ \
		echo "cd /"; \
		echo "rm bin/startx.sh"; \
		echo "write initrd/startx.sh bin/startx.sh"; \
		echo "rm bin/startw.sh"; \
		echo "write initrd/startw.sh bin/startw.sh"; \
		echo "rm etc/weston.ini"; \
		echo "write initrd/weston.ini etc/weston.ini"; \
		echo "set_inode_field bin/startx.sh mode 0100755"; \
		echo "set_inode_field bin/startw.sh mode 0100755"; \
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
	@echo "Installing Butterscotch + Undertale audio + game.unx into disk image..."
	@rm -rf /tmp/undertale-audio-unpack
	@mkdir -p /tmp/undertale-audio-unpack
	@for gz in assets/assets/*.gz; do \
		base=$$(basename "$$gz" .gz); \
		case "$$base" in game.unx) continue ;; esac; \
		gunzip -c "$$gz" > "/tmp/undertale-audio-unpack/$$base"; \
	done
	@cp -f userland/butterscotch.elf /tmp/undertale-audio-unpack/butterscotch
	@cp -f assets/game.unx /tmp/undertale-audio-unpack/game.unx
	@./scripts/populate-ext2-dir.sh ./part.img /tmp/undertale-audio-unpack opt/butterscotch
	@rm -rf /tmp/undertale-audio-unpack
	@{ \
		echo "cd /"; \
		echo "mkdir usr"; \
		echo "mkdir usr/bin"; \
		echo "rm usr/bin/butterscotch"; \
		echo "write userland/butterscotch.elf usr/bin/butterscotch"; \
		echo "set_inode_field opt/butterscotch/butterscotch mode 0100755"; \
		echo "set_inode_field usr/bin/butterscotch mode 0100755"; \
	} | debugfs -w ./part.img >/dev/null 2>&1 || true
	@echo "Fixing up glibc/musl library coexistence..."
	@{ \
		echo "cd /lib"; \
		echo "rm libc.so.6"; \
		echo "rm libm.so.6"; \
		echo "rm libpthread.so.0"; \
		echo "rm ld-linux-x86-64.so.2"; \
		echo "rm libresolv.so.2"; \
		echo "rm librt.so.1"; \
		echo "rm libutil.so.1"; \
	} | debugfs -w ./part.img >/dev/null 2>&1 || true
	@# Restore real glibc libs to /lib so glibc-linked binaries (coreutils sleep,
	@# mkdir etc.) resolve symbols correctly. musl stub at /lib/libc.so.6 lacks
	@# glibc-specific symbols like re_syntax_options which crashes those binaries.
	@if [ -f toolchain/glibc-sysroot/lib/libc.so.6 ]; then \
		echo "Restoring real glibc libc.so.6 to /lib..."; \
		debugfs -w -R "write toolchain/glibc-sysroot/lib/libc.so.6 lib/libc.so.6" ./part.img >/dev/null 2>&1 || true; \
		debugfs -w -R "write toolchain/glibc-sysroot/lib/libm.so.6 lib/libm.so.6" ./part.img >/dev/null 2>&1 || true; \
		debugfs -w -R "write toolchain/glibc-sysroot/lib/ld-linux-x86-64.so.2 lib/ld-linux-x86-64.so.2" ./part.img >/dev/null 2>&1 || true; \
	fi
	@echo "Populating root filesystem with additional tools..."

	@if [ -d build/tcc-glibc-install/opt/tcc ]; then \
		echo "Installing GLIBC TCC into disk image..."; \
		./scripts/populate-ext2-dir.sh ./part.img build/tcc-glibc-install/opt/tcc opt/tcc; \
		debugfs -w -R "rm bin/tcc" ./part.img >/dev/null 2>&1 || true; \
		debugfs -w -R "write build/tcc-glibc-install/opt/tcc/bin/tcc bin/tcc" ./part.img >/dev/null 2>&1 || true; \
		debugfs -w -R "rm lib64/libtcc.so" ./part.img >/dev/null 2>&1 || true; \
		debugfs -w -R "write build/tcc-glibc-install/opt/tcc/lib/libtcc.so lib64/libtcc.so" ./part.img >/dev/null 2>&1 || true; \
		if [ -f toolchain/musl-sysroot/lib/libc.a ]; then \
			echo "Installing musl static libs into TCC lib dir for static linking..."; \
			debugfs -w -R "mkdir opt/tcc/lib/musl" ./part.img >/dev/null 2>&1 || true; \
			debugfs -w -R "rm opt/tcc/lib/musl/libc.a" ./part.img >/dev/null 2>&1 || true; \
			debugfs -w -R "write toolchain/musl-sysroot/lib/libc.a opt/tcc/lib/musl/libc.a" ./part.img >/dev/null 2>&1 || true; \
			debugfs -w -R "rm opt/tcc/lib/musl/libm.a" ./part.img >/dev/null 2>&1 || true; \
			debugfs -w -R "write toolchain/musl-sysroot/lib/libm.a opt/tcc/lib/musl/libm.a" ./part.img >/dev/null 2>&1 || true; \
			debugfs -w -R "rm opt/tcc/lib/musl/libpthread.a" ./part.img >/dev/null 2>&1 || true; \
			debugfs -w -R "write toolchain/musl-sysroot/lib/libpthread.a opt/tcc/lib/musl/libpthread.a" ./part.img >/dev/null 2>&1 || true; \
			debugfs -w -R "rm opt/tcc/lib/musl/crt1.o" ./part.img >/dev/null 2>&1 || true; \
			debugfs -w -R "write toolchain/musl-sysroot/lib/crt1.o opt/tcc/lib/musl/crt1.o" ./part.img >/dev/null 2>&1 || true; \
			debugfs -w -R "rm opt/tcc/lib/musl/crti.o" ./part.img >/dev/null 2>&1 || true; \
			debugfs -w -R "write toolchain/musl-sysroot/lib/crti.o opt/tcc/lib/musl/crti.o" ./part.img >/dev/null 2>&1 || true; \
			debugfs -w -R "rm opt/tcc/lib/musl/crtn.o" ./part.img >/dev/null 2>&1 || true; \
			debugfs -w -R "write toolchain/musl-sysroot/lib/crtn.o opt/tcc/lib/musl/crtn.o" ./part.img >/dev/null 2>&1 || true; \
			LIBGCC_DIR=$$(find toolchain/x86_64-linux-musl/lib/gcc/x86_64-linux-musl -name "libgcc.a" -maxdepth 2 2>/dev/null | head -1 | xargs dirname 2>/dev/null); \
			if [ -n "$$LIBGCC_DIR" ] && [ -f "$$LIBGCC_DIR/libgcc.a" ]; then \
				echo "Installing libgcc.a and libgcc_eh.a into TCC lib dir..."; \
				debugfs -w -R "rm opt/tcc/lib/musl/libgcc.a" ./part.img >/dev/null 2>&1 || true; \
				debugfs -w -R "write $$LIBGCC_DIR/libgcc.a opt/tcc/lib/musl/libgcc.a" ./part.img >/dev/null 2>&1 || true; \
				if [ -f "$$LIBGCC_DIR/libgcc_eh.a" ]; then \
					debugfs -w -R "rm opt/tcc/lib/musl/libgcc_eh.a" ./part.img >/dev/null 2>&1 || true; \
					debugfs -w -R "write $$LIBGCC_DIR/libgcc_eh.a opt/tcc/lib/musl/libgcc_eh.a" ./part.img >/dev/null 2>&1 || true; \
				fi; \
			fi; \
			echo "Installing musl headers into TCC dir for static linking..."; \
			./scripts/populate-ext2-dir.sh ./part.img toolchain/musl-sysroot/include opt/tcc/lib/tcc/musl-include; \
		fi; \
	elif [ -d toolchain/musl-sysroot/opt/tcc ]; then \
		echo "Installing MUSL TCC into disk image..."; \
		./scripts/populate-ext2-dir.sh ./part.img toolchain/musl-sysroot/opt/tcc opt/tcc; \
		debugfs -w -R "write toolchain/musl-sysroot/opt/tcc/bin/tcc bin/tcc" ./part.img >/dev/null 2>&1 || true; \
		debugfs -w -R "write toolchain/musl-sysroot/lib/libc.a libc.a" ./part.img >/dev/null 2>&1 || true; \
		debugfs -w -R "write toolchain/musl-sysroot/lib/crt1.o crt1.o" ./part.img >/dev/null 2>&1 || true; \
		debugfs -w -R "write toolchain/musl-sysroot/lib/crti.o crti.o" ./part.img >/dev/null 2>&1 || true; \
		debugfs -w -R "write toolchain/musl-sysroot/lib/crtn.o crtn.o" ./part.img >/dev/null 2>&1 || true; \
		debugfs -w -R "write toolchain/musl-sysroot/opt/tcc/lib/tcc/libtcc1.a libtcc1.a" ./part.img >/dev/null 2>&1 || true; \
	fi
	@if [ -d toolchain/glibc-sysroot/opt/coreutils ]; then \
		echo "Installing glibc coreutils into disk image..."; \
		./scripts/populate-ext2-dir.sh ./part.img toolchain/glibc-sysroot/opt/coreutils opt/coreutils; \
	elif [ -d toolchain/musl-sysroot/opt/coreutils ]; then \
		echo "Installing musl coreutils into disk image..."; \
		./scripts/populate-ext2-dir.sh ./part.img toolchain/musl-sysroot/opt/coreutils opt/coreutils; \
	fi
	@if [ -d toolchain/glibc-sysroot/opt/bash ]; then \
		echo "Installing bash into disk image..."; \
		debugfs -w -R "mkdir opt" ./part.img >/dev/null 2>&1 || true; \
		./scripts/populate-ext2-dir.sh ./part.img toolchain/glibc-sysroot/opt/bash opt/bash; \
		debugfs -w -R "rm bin/bash" ./part.img >/dev/null 2>&1 || true; \
		debugfs -w -R "write toolchain/glibc-sysroot/opt/bash/bin/bash bin/bash" ./part.img >/dev/null 2>&1 || true; \
		debugfs -w -R "rm bin/sh" ./part.img >/dev/null 2>&1 || true; \
		debugfs -w -R "write toolchain/glibc-sysroot/opt/bash/bin/bash bin/sh" ./part.img >/dev/null 2>&1 || true; \
		echo "PS1='\033[0;32mRoot@AvoryOS\033[0m:\w\\$$ '" > /tmp/bashrc; \
		echo "PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/opt/coreutils/bin:/usr/bin:/sbin:/bin:/opt/bash/bin:/opt/tcc/bin" >> /tmp/bashrc; \
		echo "HOME=/" >> /tmp/bashrc; \
		echo "TERM=xterm-256color" >> /tmp/bashrc; \
		echo 'if [ -n "$$DISPLAY" ]; then unset TERM_PROGRAM TERM_PROGRAM_VERSION; else export TERM_PROGRAM=vt; fi' >> /tmp/bashrc; \
		echo "export TERM" >> /tmp/bashrc; \
		echo "SSL_CERT_FILE=/etc/ssl/certs/ca-certificates.crt" >> /tmp/bashrc; \
		echo "SSL_CERT_DIR=/etc/ssl/certs" >> /tmp/bashrc; \
		echo "CURL_CA_BUNDLE=/etc/ssl/certs/ca-certificates.crt" >> /tmp/bashrc; \
		echo "export SSL_CERT_FILE SSL_CERT_DIR CURL_CA_BUNDLE" >> /tmp/bashrc; \
		debugfs -w -R "mkdir etc" ./part.img >/dev/null 2>&1 || true; \
		echo "nameserver 10.0.2.3" > /tmp/resolv.conf; \
		echo "127.0.0.1 localhost" > /tmp/hosts; \
		debugfs -w -R "rm etc/resolv.conf" ./part.img >/dev/null 2>&1 || true; \
		debugfs -w -R "write /tmp/resolv.conf etc/resolv.conf" ./part.img >/dev/null 2>&1 || true; \
		debugfs -w -R "rm etc/hosts" ./part.img >/dev/null 2>&1 || true; \
		debugfs -w -R "write /tmp/hosts etc/hosts" ./part.img >/dev/null 2>&1 || true; \
		debugfs -w -R "mkdir etc/ssl" ./part.img >/dev/null 2>&1 || true; \
		debugfs -w -R "mkdir etc/ssl/certs" ./part.img >/dev/null 2>&1 || true; \
		debugfs -w -R "rm etc/ssl/certs/ca-certificates.crt" ./part.img >/dev/null 2>&1 || true; \
		debugfs -w -R "write build/alpine/rootfs/etc/ssl/certs/ca-certificates.crt etc/ssl/certs/ca-certificates.crt" ./part.img >/dev/null 2>&1 || true; \
		echo "NAME=\"AvoryOS\"" > /tmp/os-release; \
		echo "ID=avoryos" >> /tmp/os-release; \
		echo "VERSION=\"2.5.0 Beta\"" >> /tmp/os-release; \
		echo "VERSION_ID=2.5.0-beta" >> /tmp/os-release; \
		echo "PRETTY_NAME=\"AvoryOS 2.5.0 Beta x86_64\"" >> /tmp/os-release; \
		echo "HOME_URL=\"https://github.com/Hidotu-Labs/AvoryOS\"" >> /tmp/os-release; \
		debugfs -w -R "rm etc/os-release" ./part.img >/dev/null 2>&1 || true; \
		debugfs -w -R "write /tmp/os-release etc/os-release" ./part.img >/dev/null 2>&1 || true; \
		debugfs -w -R "rm usr/lib/os-release" ./part.img >/dev/null 2>&1 || true; \
		debugfs -w -R "write /tmp/os-release usr/lib/os-release" ./part.img >/dev/null 2>&1 || true; \
		debugfs -w -R "rm .bashrc" ./part.img >/dev/null 2>&1 || true; \
		debugfs -w -R "write /tmp/bashrc .bashrc" ./part.img >/dev/null 2>&1 || true; \
		debugfs -w -R "rm etc/profile" ./part.img >/dev/null 2>&1 || true; \
		debugfs -w -R "write /tmp/bashrc etc/profile" ./part.img >/dev/null 2>&1 || true; \
		debugfs -w -R "rm etc/bash.bashrc" ./part.img >/dev/null 2>&1 || true; \
		debugfs -w -R "write /tmp/bashrc etc/bash.bashrc" ./part.img >/dev/null 2>&1 || true; \
		debugfs -w -R "mkdir .config" ./part.img >/dev/null 2>&1 || true; \
		debugfs -w -R "mkdir .config/fastfetch" ./part.img >/dev/null 2>&1 || true; \
		debugfs -w -R "mkdir fastfetch" ./part.img >/dev/null 2>&1 || true; \
		echo '{"general": {"detectVersion": false}, "logo": {"source": "/fastfetch/logo.txt", "type": "auto"}, "modules": ["title", "separator", "os", "kernel", "uptime", "packages", {"type": "shell", "format": "bash"}, "de", "wm", "terminal", "cursor", "cpu", {"type": "custom", "key": "GPU", "format": "VirtIO-GPU (virtio-vga, 2D) / Mesa llvmpipe (software 3D)"}, "memory", "swap", "disk", {"type": "localip", "key": "Local IP", "showIpv6": false}, "locale", "break", "colors"]}' > /tmp/ff_config.jsonc; \
		debugfs -w -R "rm .config/fastfetch/config.jsonc" ./part.img >/dev/null 2>&1 || true; \
		debugfs -w -R "write /tmp/ff_config.jsonc .config/fastfetch/config.jsonc" ./part.img >/dev/null 2>&1 || true; \
		debugfs -w -R "rm fastfetch/config.jsonc" ./part.img >/dev/null 2>&1 || true; \
		debugfs -w -R "write /tmp/ff_config.jsonc fastfetch/config.jsonc" ./part.img >/dev/null 2>&1 || true; \
		debugfs -w -R "rm fastfetch/logo.txt" ./part.img >/dev/null 2>&1 || true; \
		debugfs -w -R "write assets/ascii-art.txt fastfetch/logo.txt" ./part.img >/dev/null 2>&1 || true; \
		rm -f /tmp/bashrc /tmp/resolv.conf /tmp/hosts /tmp/ff_config.jsonc /tmp/os-release; \
	fi
	@if [ -f userland/winoptions ] && [ -f userland/icewm-menu ]; then \
		echo "Installing IceWM configuration into disk image..."; \
		debugfs -w -R "mkdir etc/icewm" ./part.img >/dev/null 2>&1 || true; \
		debugfs -w -R "rm etc/icewm/winoptions" ./part.img >/dev/null 2>&1 || true; \
		debugfs -w -R "write userland/winoptions etc/icewm/winoptions" ./part.img >/dev/null 2>&1 || true; \
		debugfs -w -R "rm etc/icewm/menu" ./part.img >/dev/null 2>&1 || true; \
		debugfs -w -R "write userland/icewm-menu etc/icewm/menu" ./part.img >/dev/null 2>&1 || true; \
	fi
	@if [ -f toolchain/musl-sysroot/bin/tar ]; then \
		echo "Installing tar into disk image..."; \
		debugfs -w -R "rm bin/tar" ./part.img >/dev/null 2>&1 || true; \
		debugfs -w -R "write toolchain/musl-sysroot/bin/tar bin/tar" ./part.img >/dev/null 2>&1 || true; \
	fi
	@if [ -f userland/doom_x11.elf ]; then \
		echo "Installing doom_x11 into disk image..."; \
		debugfs -w -R "rm bin/doom_x11" ./part.img >/dev/null 2>&1 || true; \
		debugfs -w -R "write userland/doom_x11.elf bin/doom_x11" ./part.img >/dev/null 2>&1 || true; \
	fi
	@if [ -f userland/qt5_test.elf ]; then \
		echo "Installing qt5_test into disk image..."; \
		debugfs -w -R "rm bin/qt5_test" ./part.img >/dev/null 2>&1 || true; \
		debugfs -w -R "write userland/qt5_test.elf bin/qt5_test" ./part.img >/dev/null 2>&1 || true; \
	fi
	@if [ -f userland/sdl3_test.elf ]; then \
		echo "Installing sdl3_test into disk image..."; \
		debugfs -w -R "rm bin/sdl3_test" ./part.img >/dev/null 2>&1 || true; \
		debugfs -w -R "write userland/sdl3_test.elf bin/sdl3_test" ./part.img >/dev/null 2>&1 || true; \
	fi

	@echo "Fixing executable modes for directly injected launchers..."
	@{ \
		echo "rm bin/ls"; \
		echo "symlink bin/ls /opt/coreutils/bin/ls"; \
		echo "set_inode_field bin/bash mode 0100755"; \
		echo "set_inode_field bin/sh mode 0100755"; \
		echo "set_inode_field bin/avory-login mode 0100755"; \
		echo "set_inode_field bin/avoryd mode 0100755"; \
		echo "set_inode_field bin/sax11.sh mode 0100755"; \
		echo "set_inode_field bin/sawayland.sh mode 0100755"; \
		echo "set_inode_field bin/AetherWM mode 0100755"; \
		echo "set_inode_field bin/aether-dock mode 0100755"; \
		echo "set_inode_field bin/aether-panel mode 0100755"; \
		echo "set_inode_field bin/aether-compositor mode 0100755"; \
		echo "set_inode_field bin/aether-window mode 0100755"; \
		echo "set_inode_field bin/forkit mode 0100755"; \
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
		echo "set_inode_field bin/test_heap_smp mode 0100755"; \
		echo "set_inode_field bin/test_dcache mode 0100755"; \
		echo "set_inode_field bin/test_tmpfile mode 0100755"; \
		echo "set_inode_field bin/test_kpi_dmabuf mode 0100755"; \
		echo "set_inode_field bin/test_kpi_drm mode 0100755"; \
		echo "set_inode_field bin/test_kpi_bochs mode 0100755"; \
		echo "set_inode_field bin/test_child_notify mode 0100755"; \
		echo "set_inode_field bin/test_pty_master mode 0100755"; \
		echo "set_inode_field bin/test_uaccess_bench mode 0100755"; \
		echo "set_inode_field bin/test_readahead mode 0100755"; \
		echo "set_inode_field home/avory uid 1000"; \
		echo "set_inode_field home/avory gid 1000"; \
	} | debugfs -w ./part.img >/dev/null 2>&1 || true

	@echo "Creating partitioned disk image (MBR)..."
	dd if=/dev/zero of=disk.img bs=1M count=5120
	echo '2048,,L,*' | sfdisk disk.img >/dev/null 2>&1 || (parted -s disk.img mklabel msdos && parted -s disk.img mkpart primary ext3 1MiB 100% && parted -s disk.img set 1 boot on)
	dd if=./part.img of=disk.img bs=1M seek=1 conv=notrunc
	rm -f ./part.img
	@touch disk.img

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
setup:
	chmod +x bootstrap.sh
	./bootstrap.sh

.PHONY: kernel
kernel: setup
	$(MAKE) -C kernel

$(IMAGE_NAME).iso: limine/limine kernel limine.conf
	rm -rf iso_root
	mkdir -p iso_root/boot
	cp -v kernel/bin-$(ARCH)/kernel iso_root/boot/
	mkdir -p iso_root/boot/limine
	# The regular ISO expects disk.img as a separate QEMU drive. Only the
	# self-contained dist ISO copies the module-enabled configuration verbatim.
	sed '/^[[:space:]]*module_path: boot():\/disk.img$$/d; /^[[:space:]]*module_string: disk.img$$/d' \
		limine.conf > iso_root/boot/limine/limine.conf
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
	rm -rf iso_root

.PHONY: clean
clean:
	$(MAKE) -C kernel clean
	rm -f $(IMAGE_NAME).iso

.PHONY: clean-all
clean-all: clean-musl clean-doom clean-coreutils clean-tar clean-apm
	$(MAKE) -C kernel clean
	rm -rf iso_root $(IMAGE_NAME).iso $(IMAGE_NAME).hdd build/alpine

.PHONY: clean-coreutils
clean-coreutils:
	rm -rf build/coreutils-9.5
	rm -rf toolchain/musl-sysroot/opt/coreutils
	rm -rf toolchain/glibc-sysroot/opt/coreutils

.PHONY: clean-musl
clean-musl:
	rm -rf build/musl-1.2.5 build/musl-cross-make
	rm -rf toolchain/musl-sysroot toolchain/x86_64-linux-musl
	rm -f userland/hello.elf userland/avoryd.elf  userland/kilo.elf userland/kilo.c  userland/asplay.elf userland/kria.elf userland/ls.elf userland/readelf.elf userland/poll_test.elf
	rm -rf userland/kria-lang/target

.PHONY: clean-apm
clean-apm:
	rm -f userland/apm.elf

.PHONY: clean-disk
clean-disk:
	rm -f disk.img

.PHONY: distclean
distclean: clean-musl clean-doom
	$(MAKE) -C kernel distclean
	rm -rf iso_root *.iso *.hdd limine edk2-ovmf doomgeneric
	rm -rf userland/kria-lang

# ── Userland test programs ──────────────────────────────────────────────────
$(MUSL_LIBC):
	chmod +x scripts/musl-toolchain.sh
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" ./scripts/musl-toolchain.sh

.PHONY: musl-toolchain
musl-toolchain: $(MUSL_LIBC)

.PHONY: test-phase6-login
test-phase6-login:
	./scripts/test-phase6-login.sh

userland/avoryd.elf: userland/avoryd.c $(MUSL_LIBC)
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" $(MUSL_CC) $(MUSL_USER_CFLAGS) \
		userland/avoryd.c -o userland/avoryd.elf

userland/avory-login.elf: userland/avory-login.c $(MUSL_LIBC)
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" $(MUSL_CC) $(MUSL_USER_CFLAGS) \
		userland/avory-login.c -lcrypt -o userland/avory-login.elf

userland/nvme_test.elf: userland/nvme_test.c $(MUSL_LIBC)
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" $(MUSL_CC) $(MUSL_USER_CFLAGS) \
		userland/nvme_test.c -o userland/nvme_test.elf -lpthread -lm

userland/nvme_bench.elf: userland/nvme_bench.c $(MUSL_LIBC)
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" $(MUSL_CC) $(MUSL_USER_CFLAGS) \
		userland/nvme_bench.c -o userland/nvme_bench.elf -lpthread

userland/hello_glibc.elf: userland/hello_glibc.c
	$(GLIBC_CC) $(GLIBC_USER_CFLAGS) \
		userland/hello_glibc.c -o userland/hello_glibc.elf

userland/test_cpp.elf: userland/test_cpp.cpp $(MUSL_LIBC)
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" $(MUSL_CXX) $(MUSL_USER_CXXFLAGS) \
		userland/test_cpp.cpp -o userland/test_cpp.elf


userland/kilo.c:
	curl -L https://raw.githubusercontent.com/antirez/kilo/master/kilo.c -o userland/kilo.c

userland/kilo.elf: userland/kilo.c $(MUSL_LIBC)
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" $(MUSL_CC) $(MUSL_USER_CFLAGS) \
		userland/kilo.c -o userland/kilo.elf

userland/ls.elf: userland/ls.c $(MUSL_LIBC)
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" $(MUSL_CC) $(MUSL_USER_CFLAGS) \
		userland/ls.c -o userland/ls.elf

userland/lspci.elf: userland/lspci.c $(MUSL_LIBC)
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" $(MUSL_CC) $(MUSL_USER_CFLAGS) \
		userland/lspci.c -o userland/lspci.elf

userland/lsblk.elf: userland/lsblk.c $(MUSL_LIBC)
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" $(MUSL_CC) $(MUSL_USER_CFLAGS) \
		userland/lsblk.c -o userland/lsblk.elf

userland/readelf.elf: userland/readelf.c $(MUSL_LIBC)
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" $(MUSL_CC) $(MUSL_USER_CFLAGS) \
		userland/readelf.c -o userland/readelf.elf

userland/pong.elf: userland/pong.c $(MUSL_LIBC)
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" $(MUSL_CC) $(MUSL_USER_CFLAGS) \
		userland/pong.c -o userland/pong.elf \
		-lm

userland/raycast.elf: userland/raycast.c $(MUSL_LIBC)
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" $(MUSL_CC) $(MUSL_USER_CFLAGS) \
		userland/raycast.c -lm -o userland/raycast.elf

userland/asplay.elf: userland/asplay.c $(MUSL_LIBC)
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" $(MUSL_CC) $(MUSL_USER_CFLAGS) \
		userland/asplay.c -o userland/asplay.elf

userland/booter.elf: userland/booter.c $(MUSL_LIBC)
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" $(MUSL_CC) $(MUSL_USER_CFLAGS) \
		userland/booter.c -o userland/booter.elf

userland/reboot.elf: userland/reboot.c $(MUSL_LIBC)
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" $(MUSL_CC) $(MUSL_USER_CFLAGS) \
		userland/reboot.c -o userland/reboot.elf

userland/shutdown.elf: userland/shutdown.c $(MUSL_LIBC)
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" $(MUSL_CC) $(MUSL_USER_CFLAGS) \
		userland/shutdown.c -o userland/shutdown.elf

# APM package manager
# apm.c shells out to gzip/tar for APK extraction — no zlib C API is used.
userland/apm.elf: userland/apm.c $(MUSL_LIBC)
	@echo "[*] Building APM package manager..."
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" $(MUSL_CC) $(MUSL_USER_CFLAGS) \
		userland/apm.c -o userland/apm.elf

userland/dns_lookup.elf: userland/dns_lookup.c userland/dns_resolver.c userland/dns_resolver.h $(MUSL_LIBC)
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" $(MUSL_CC) $(MUSL_USER_CFLAGS) \
		userland/dns_lookup.c userland/dns_resolver.c -o userland/dns_lookup.elf

userland/nettest.elf: userland/nettest.c $(MUSL_LIBC)
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" $(MUSL_CC) $(MUSL_USER_CFLAGS) \
		userland/nettest.c -o userland/nettest.elf

# Kria programming language (Rust-based, compiled with musl for static linking)
userland/kria-lang:
	rm -rf userland/kria-lang
	git clone https://github.com/Piotriox/kria-lang.git userland/kria-lang
	mkdir -p userland/kria-lang/.cargo
	echo '[build]' > userland/kria-lang/.cargo/config.toml
	echo 'target = "x86_64-unknown-linux-musl"' >> userland/kria-lang/.cargo/config.toml

userland/kria.elf: userland/kria-lang
	cd userland/kria-lang && cargo build --release
	cp userland/kria-lang/target/x86_64-unknown-linux-musl/release/kria userland/kria.elf

.PHONY: kria
kria: userland/kria.elf

# ── DOOM (doomgeneric) ──────────────────────────────────────────────────────
.PHONY: doom
doom: userland/doom.elf

doomgeneric:
	rm -rf doomgeneric
	git clone https://github.com/ozkl/doomgeneric.git --depth=1

userland/doom.elf userland/doom_x11.elf: doomgeneric $(MUSL_LIBC) $(ALPINE_STAMP)
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" $(MAKE) -C userland -f Makefile.avoryos \
		MUSL_CC="$(MUSL_TOOLCHAIN_BIN)/x86_64-linux-musl-gcc" \
		MUSL_SYSROOT="$(MUSL_SYSROOT)" \
		ALPINE_SYSROOT="$(ALPINE_SYSROOT)"

.PHONY: clean-doom
clean-doom:
	$(MAKE) -C userland -f Makefile.avoryos clean

userland/gtk_test.elf: userland/gtk_test.c $(ALPINE_STAMP) $(MUSL_LIBC)
	@echo "[*] Compiling userland/gtk_test.c (GTK2) ..."
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" \
		$(MUSL_TOOLCHAIN_BIN)/x86_64-linux-musl-gcc -O2 \
		userland/gtk_test.c \
		-o userland/gtk_test.elf \
		$(GTK2_INCLUDES) \
		$(GTK2_LIBS) \
		$(GTK2_LDFLAGS)

userland/gtk3_test.elf: userland/gtk3_test.c $(ALPINE_STAMP) $(MUSL_LIBC)
	@echo "[*] Compiling userland/gtk3_test.c (GTK3) ..."
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" \
		$(MUSL_TOOLCHAIN_BIN)/x86_64-linux-musl-gcc -O2 \
		userland/gtk3_test.c \
		-o userland/gtk3_test.elf \
		$(GTK3_INCLUDES) \
		$(GTK3_LIBS) \
		$(GTK3_LDFLAGS)

userland/sdl3_test.elf: userland/sdl3_test.c $(ALPINE_STAMP) $(MUSL_LIBC)
	@echo "[*] Compiling userland/sdl3_test.c (SDL3 + OpenGL + libplacebo + Capstone) ..."
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" \
		$(MUSL_TOOLCHAIN_BIN)/x86_64-linux-musl-gcc -O2 \
		userland/sdl3_test.c \
		-o userland/sdl3_test.elf \
		$(SDL3_INCLUDES) \
		$(SDL3_LIBS) \
		$(SDL3_LDFLAGS)

userland/qt5_test.elf: userland/qt5_test.cpp $(ALPINE_STAMP) $(MUSL_LIBC)
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" \
		$(MUSL_TOOLCHAIN_BIN)/x86_64-linux-musl-g++ -O2 -std=c++14 \
		userland/qt5_test.cpp \
		-o userland/qt5_test.elf \
		$(QT5_CXXFLAGS) \
		$(QT5_INCLUDES) \
		$(QT5_LIBS) \
		$(QT5_LDFLAGS)

TINYGL_LIB := $(MUSL_SYSROOT)/opt/tinygl/lib/libTinyGL.a

$(TINYGL_LIB): scripts/build-tinygl.sh $(MUSL_LIBC)
	@echo "[*] Building TinyGL library..."
	chmod +x scripts/build-tinygl.sh
	./scripts/build-tinygl.sh

.PHONY: tinygl
tinygl: $(TINYGL_LIB)

userland/tglgears_fb.elf: userland/tglgears_fb.c $(MUSL_LIBC) $(TINYGL_LIB)
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" $(MUSL_CC) $(MUSL_USER_CFLAGS) \
		userland/tglgears_fb.c -I$(MUSL_SYSROOT)/opt/tinygl/include -L$(MUSL_SYSROOT)/opt/tinygl/lib -lTinyGL -L$(MUSL_SYSROOT)/lib -lm -o userland/tglgears_fb.elf

userland/test_mem_stress.elf: userland/test_mem_stress.c $(MUSL_LIBC)
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" $(MUSL_CC) $(MUSL_USER_CFLAGS) \
		userland/test_mem_stress.c -o userland/test_mem_stress.elf

userland/test_clone_futex.elf: userland/test_clone_futex.c userland/test_clone_futex_trampoline.S $(MUSL_LIBC)
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" $(MUSL_CC) $(MUSL_USER_CFLAGS) \
		userland/test_clone_futex.c userland/test_clone_futex_trampoline.S -o userland/test_clone_futex.elf

userland/test_futex_pi.elf: userland/test_futex_pi.c $(MUSL_LIBC)
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" $(MUSL_CC) $(MUSL_USER_CFLAGS) \
		userland/test_futex_pi.c -o userland/test_futex_pi.elf -lpthread -lm

userland/test_unix_sockets.elf: userland/test_unix_sockets.c $(MUSL_LIBC)
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" $(MUSL_CC) $(MUSL_USER_CFLAGS) \
		userland/test_unix_sockets.c -o userland/test_unix_sockets.elf

userland/test_tmpfile.elf: userland/test_tmpfile.c $(MUSL_LIBC)
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" $(MUSL_CC) $(MUSL_USER_CFLAGS) \
		userland/test_tmpfile.c -o userland/test_tmpfile.elf

# Phase 2 LinuxKPI /dev/kpi_dmabuf test; shares the device ABI header with the
# kernel side (kernel/linuxkpi/include/uapi/kpi_dmabuf.h).
userland/test_kpi_dmabuf.elf: userland/test_kpi_dmabuf.c $(MUSL_LIBC) \
		kernel/linuxkpi/include/uapi/kpi_dmabuf.h
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" $(MUSL_CC) $(MUSL_USER_CFLAGS) \
		-I$(CURDIR)/kernel/linuxkpi/include \
		userland/test_kpi_dmabuf.c -o userland/test_kpi_dmabuf.elf -lpthread

# Phase 3 LinuxKPI DRM test: vkms dumb-buffer GEM mmap/write, renderD128
# sanity and a modetest-lite atomic enable/disable.  Uses the musl sysroot's
# Linux UAPI drm headers.
userland/test_kpi_drm.elf: userland/test_kpi_drm.c $(MUSL_LIBC)
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" $(MUSL_CC) $(MUSL_USER_CFLAGS) \
		userland/test_kpi_drm.c -o userland/test_kpi_drm.elf

# Phase 4 C5 bochs canary: card discovery by DRM version name, dumb-buffer
# mmap write/read and a full-CRTC atomic enable/disable.
userland/test_kpi_bochs.elf: userland/test_kpi_bochs.c $(MUSL_LIBC)
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" $(MUSL_CC) $(MUSL_USER_CFLAGS) \
		userland/test_kpi_bochs.c -o userland/test_kpi_bochs.elf

userland/test_child_notify.elf: userland/test_child_notify.c $(MUSL_LIBC)
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" $(MUSL_CC) $(MUSL_USER_CFLAGS) \
		userland/test_child_notify.c -o userland/test_child_notify.elf

userland/test_pty_master.elf: userland/test_pty_master.c $(MUSL_LIBC)
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" $(MUSL_CC) $(MUSL_USER_CFLAGS) \
		userland/test_pty_master.c -o userland/test_pty_master.elf

userland/test_syscall_speed.elf: userland/test_syscall_speed.c $(MUSL_LIBC)
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" $(MUSL_CC) $(MUSL_USER_CFLAGS) \
		userland/test_syscall_speed.c -o userland/test_syscall_speed.elf

userland/test_hugepages.elf: userland/test_hugepages.c $(MUSL_LIBC)
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" $(MUSL_CC) $(MUSL_USER_CFLAGS) \
		userland/test_hugepages.c -o userland/test_hugepages.elf

userland/test_zero_page.elf: userland/test_zero_page.c $(MUSL_LIBC)
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" $(MUSL_CC) $(MUSL_USER_CFLAGS) \
		userland/test_zero_page.c -o userland/test_zero_page.elf

userland/test_watchdog.elf: userland/test_watchdog.c $(MUSL_LIBC)
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" $(MUSL_CC) $(MUSL_USER_CFLAGS) \
		userland/test_watchdog.c -o userland/test_watchdog.elf

userland/test_copy_user.elf: userland/test_copy_user.c $(MUSL_LIBC)
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" $(MUSL_CC) $(MUSL_USER_CFLAGS) \
		userland/test_copy_user.c -o userland/test_copy_user.elf

userland/test_pcp_pmm.elf: userland/test_pcp_pmm.c $(MUSL_LIBC)
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" $(MUSL_CC) $(MUSL_USER_CFLAGS) \
		userland/test_pcp_pmm.c -o userland/test_pcp_pmm.elf -lpthread -lm

userland/test_ticket_lock.elf: userland/test_ticket_lock.c $(MUSL_LIBC)
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" $(MUSL_CC) $(MUSL_USER_CFLAGS) \
		userland/test_ticket_lock.c -o userland/test_ticket_lock.elf -lpthread -lm

userland/test_fb_perf.elf: userland/test_fb_perf.c $(MUSL_LIBC)
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" $(MUSL_CC) $(MUSL_USER_CFLAGS) \
		userland/test_fb_perf.c -o userland/test_fb_perf.elf -lpthread -lm

userland/test_vdso_bench.elf: userland/test_vdso_bench.c $(MUSL_LIBC)
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" $(MUSL_CC) $(MUSL_USER_CFLAGS) \
		userland/test_vdso_bench.c -o userland/test_vdso_bench.elf

userland/test_lazy_fpu.elf: userland/test_lazy_fpu.c $(MUSL_LIBC)
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" $(MUSL_CC) $(MUSL_USER_CFLAGS) \
		userland/test_lazy_fpu.c -o userland/test_lazy_fpu.elf -lpthread -lm

userland/test_heap_smp.elf: userland/test_heap_smp.c $(MUSL_LIBC)
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" $(MUSL_CC) $(MUSL_USER_CFLAGS) \
		userland/test_heap_smp.c -o userland/test_heap_smp.elf -lpthread -lm

userland/test_dcache.elf: userland/test_dcache.c $(MUSL_LIBC)
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" $(MUSL_CC) $(MUSL_USER_CFLAGS) \
		userland/test_dcache.c -o userland/test_dcache.elf

userland/test_uaccess_bench.elf: userland/test_uaccess_bench.c $(MUSL_LIBC)
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" $(MUSL_CC) $(MUSL_USER_CFLAGS) \
		userland/test_uaccess_bench.c -o userland/test_uaccess_bench.elf

userland/test_readahead.elf: userland/test_readahead.c $(MUSL_LIBC)

	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" $(MUSL_CC) $(MUSL_USER_CFLAGS) \
		userland/test_readahead.c -o userland/test_readahead.elf

userland/proc_bench.elf: userland/proc_bench.c userland/proc_bench_trampoline.S $(MUSL_LIBC)



	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" $(MUSL_CC) $(MUSL_USER_CFLAGS) \
		userland/proc_bench.c userland/proc_bench_trampoline.S -o userland/proc_bench.elf


userland/panic_test.elf: userland/panic_test.c $(MUSL_LIBC)
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" $(MUSL_CC) $(MUSL_USER_CFLAGS) \
		userland/panic_test.c -o userland/panic_test.elf

userland/fault_mon.elf: userland/fault_mon.c $(MUSL_LIBC)
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" $(MUSL_CC) $(MUSL_USER_CFLAGS) \
		userland/fault_mon.c -o userland/fault_mon.elf

userland/tglgears_drm.elf: userland/tglgears_drm.c $(MUSL_LIBC) $(TINYGL_LIB)
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" $(MUSL_CC) $(MUSL_USER_CFLAGS) \
		userland/tglgears_drm.c -I$(MUSL_SYSROOT)/opt/tinygl/include -L$(MUSL_SYSROOT)/opt/tinygl/lib -lTinyGL -lm -o userland/tglgears_drm.elf

userland/tglhello_drm.elf: userland/tglhello_drm.c $(MUSL_LIBC) $(TINYGL_LIB)
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" $(MUSL_CC) $(MUSL_USER_CFLAGS) \
		userland/tglhello_drm.c -I$(MUSL_SYSROOT)/opt/tinygl/include -L$(MUSL_SYSROOT)/opt/tinygl/lib -lTinyGL -lm -o userland/tglhello_drm.elf

userland/classicube.elf: $(ALPINE_STAMP) $(MUSL_LIBC) scripts/build-classicube.sh
	./scripts/build-classicube.sh

userland/forkit.elf: $(MUSL_LIBC) scripts/build-forkit.sh
	./scripts/build-forkit.sh

userland/about.elf: userland/about.c scripts/build-about.sh $(ALPINE_STAMP) $(MUSL_LIBC)
	./scripts/build-about.sh

userland/texpacks/classicube.zip:
	@echo "ERROR: userland/texpacks/classicube.zip not found."
	@echo "Please place the original ClassiCube texture pack zip at: userland/texpacks/classicube.zip"
	@exit 1

userland/terrain.png: userland/texpacks/classicube.zip
	@echo "Extracting terrain.png from classicube.zip..."
	cd userland && unzip -o texpacks/classicube.zip terrain.png

.PHONY: all qemu clean
