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
QEMUFLAGS := -m 4G \
	-vga none \
	-device virtio-vga,xres=1280,yres=800 \
	-display gtk,zoom-to-fit=off,grab-on-hover=on

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

$(QUAKE2_STAMP): $(ALPINE_STAMP) $(GLIBC_STAMP) scripts/build-quake2.sh \
		scripts/quake2-sdl2-config.in scripts/quake2-avoryos-evdev.h
	./scripts/build-quake2.sh && \
		mkdir -p $(dir $(QUAKE2_STAMP)) && \
		touch $(QUAKE2_STAMP)

BUTTERSCOTCH_STAMP := build/butterscotch/.built

$(BUTTERSCOTCH_STAMP): $(ALPINE_STAMP) $(GLIBC_STAMP) scripts/build-butterscotch.sh
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

# Alpine rootfs sysroot (built by scripts/setup-arch.sh)
ALPINE_SYSROOT := $(CURDIR)/build/alpine/rootfs
ALPINE_STAMP := $(CURDIR)/build/alpine/.built

$(ALPINE_STAMP): scripts/setup-arch.sh
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
	-I$(ALPINE_SYSROOT)/usr/include/qt \
	-I$(ALPINE_SYSROOT)/usr/include/qt/QtCore \
	-I$(ALPINE_SYSROOT)/usr/include/qt/QtGui \
	-I$(ALPINE_SYSROOT)/usr/include/qt/QtWidgets \
	-I$(ALPINE_SYSROOT)/usr/include/qt/QtOpenGL \
	-I$(ALPINE_SYSROOT)/usr/include/qt/QtPrintSupport
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
RUN_SMP ?= 8
.PHONY: run run-tcg
run: edk2-ovmf $(IMAGE_NAME).iso disk.img
	qemu-system-$(ARCH) \
		-M q35,pcspk-audiodev=snd0 \
		-drive if=pflash,unit=0,format=raw,file=edk2-ovmf/ovmf-code-$(ARCH).fd,readonly=on \
		-cdrom $(IMAGE_NAME).iso \
		-drive file=disk.img,format=raw,if=none,id=nvme0 \
		-device nvme,serial=avoryos0,drive=nvme0 \
		-cpu host -enable-kvm \
		-smp $(RUN_SMP) \
		-serial stdio \
		-audiodev pa,id=snd0,timer-period=2000,out.frequency=48000,out.channels=2,out.format=s16,out.buffer-length=$(AUDIO_BUFLEN),out.latency=$(AUDIO_LATENCY) \
		-device rtl8139,netdev=net0 \
		-netdev user,id=net0 \
		-device intel-hda -device hda-duplex,audiodev=snd0 \
		-device qemu-xhci,id=xhci \
		-device usb-kbd,bus=xhci.0 \
		-device usb-tablet,bus=xhci.0 \
		$(QEMUFLAGS)
run-tcg: run-x86_64

# ── NUMA multi-node test target ──────────────────────────────────────────────
#
# Boots AvoryOS with 2 NUMA nodes, 4 vCPUs (2 per node), 4GB RAM (2GB per node),
# and explicit inter-node distance matrix.
.PHONY: run-numa
run-numa: edk2-ovmf $(IMAGE_NAME).iso disk.img
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
		-device rtl8139,netdev=net0 \
		-netdev user,id=net0 \
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
VFIO_MEM ?= 4G
VFIO_SMP ?= 8
VFIO_ROM ?= build/vfio/vbios.rom
# Extra QEMU -device arguments for a run-vfio boot, e.g.
#   make run-vfio VFIO_EXTRA='-device edu'
VFIO_EXTRA ?=
# Absolute pointing for ordinary VFIO desktops; PRIME games need relative motion.
VFIO_POINTER ?= usb-tablet
# Extra raw QEMU arguments for a run-vfio boot, e.g. to capture the exceptions
# around a silent triple-fault reset (the VM stops at the fault instead of
# rebooting):
#   make run-c7-prime QEMU_EXTRA='-no-reboot -d int,cpu_reset,guest_errors -D build/logs/prime-qemu.log'
QEMU_EXTRA ?=
VFIO_COMMA := ,
VFIO_ROM_OPT = $(if $(wildcard $(VFIO_ROM)),$(VFIO_COMMA)romfile=$(VFIO_ROM),)

.PHONY: run-vfio
run-vfio: QEMUFLAGS = -vga none -device virtio-vga,addr=06.0,xres=1280,yres=800
run-vfio: edk2-ovmf $(IMAGE_NAME).iso disk.img
	qemu-system-$(ARCH) \
		-M q35 \
		-m $(VFIO_MEM) \
		-cpu host -enable-kvm \
		-smp $(VFIO_SMP) \
		-drive if=pflash,unit=0,format=raw,file=edk2-ovmf/ovmf-code-$(ARCH).fd,readonly=on \
		-cdrom $(IMAGE_NAME).iso \
		-drive file=disk.img,format=raw,if=none,id=nvme0 \
		-device nvme,serial=avoryos0,drive=nvme0 \
		-device vfio-pci,host=$(VFIO_BDF),addr=03.0,rombar=1$(VFIO_ROM_OPT) \
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
.PHONY: run-c7-prime
run-c7-prime: KERNEL_CMDLINE = kpi_amdgpu=1 kpi_prime_offload=1 amdgpu.runpm=0 amdgpu.ppfeaturemask=0xfff73fff drm.debug=0x4
run-c7-prime: SERIAL = chardev:prime-diag
run-c7-prime: VFIO_SMP = 8
# GLFW cursor capture/raw motion requires a relative device, not a tablet.
run-c7-prime: VFIO_POINTER = usb-mouse
run-c7-prime: GTK_SHOW_CURSOR = off
run-c7-prime: VFIO_EXTRA = -device edu -chardev socket,id=prime-diag,path=build/logs/p6-c7-prime-serial.sock,server=on,wait=off,logfile=build/logs/p6-c7-prime.log -device rtl8139,netdev=net0,addr=08.0 -netdev user,id=net0
run-c7-prime: QEMUFLAGS = -vga none -device virtio-vga,addr=06.0,xres=1280,yres=800
run-c7-prime: run-vfio

# Separate, explicitly invoked host reset helper. run-c7-prime does not call
# it. Closing/reopening QEMU can exercise VFIO reset and power paths; neither
# a VM restart nor a successful reset request guarantees cold GPU state.
# The Raphael GPU has bus-only reset and shares its bridge with host devices.
# Do not use reset-gpu as an automatic init-retry step; see the incident notes
# in docs/amdgpu-testing.md. No host reset/power experiment is authorized here.
.PHONY: reset-gpu
reset-gpu:
	sudo scripts/vfio-reset-gpu.sh $(VFIO_BDF)

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
# bash + GNU coreutils + Xorg + IceWM + OpenRC/D-Bus + st, without the full
# desktop stack.  scripts/build-minimal.sh assembles an independent rootfs and
# disk image under build/minimal/; create_dist_usb.sh packages it as a
# self-contained ISO (the disk image is embedded as a Limine module, exactly
# like avoryos-dist.iso).
MINIMAL_ISO := avoryos-minimal.iso
MINIMAL_DISK := build/minimal/disk.img
MINIMAL_STAMP := build/minimal/rootfs/.avoryos-minimal

$(MINIMAL_STAMP): scripts/build-minimal.sh
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
		-device rtl8139,netdev=net0 \
		-netdev user,id=net0 \
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
run-sata: edk2-ovmf $(IMAGE_NAME).iso disk.img
	qemu-system-$(ARCH) \
		-M q35,pcspk-audiodev=snd0 \
		-drive if=pflash,unit=0,format=raw,file=edk2-ovmf/ovmf-code-$(ARCH).fd,readonly=on \
		-cdrom $(IMAGE_NAME).iso \
		-drive file=disk.img,format=raw,if=none,id=disk0 -device ide-hd,drive=disk0,bus=ide.0 \
		-cpu host -enable-kvm \
		-smp 4 \
		-serial stdio \
		-audiodev pa,id=snd0,timer-period=2000,out.frequency=48000,out.channels=2,out.format=s16,out.buffer-length=$(AUDIO_BUFLEN),out.latency=$(AUDIO_LATENCY) \
		-device rtl8139,netdev=net0 \
		-netdev user,id=net0 \
		-device intel-hda -device hda-duplex,audiodev=snd0 \
		-device qemu-xhci,id=xhci \
		-device usb-kbd,bus=xhci.0 \
		-device usb-tablet,bus=xhci.0 \
		$(QEMUFLAGS)

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
		-device usb-tablet,bus=xhci.0 \
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
build/test_fw.bin:
	@mkdir -p build
	@python3 -c 'import sys; open(sys.argv[1], "wb").write(bytes(((i * 7 + 3) & 0xff) for i in range(4096)))' $@

# Phase 6 C4: amdgpu firmware blobs.  scripts/linux-firmware-install.sh follows
# scripts/linux/firmware-manifest.txt and stages the pinned linux-firmware ref
# (the script's default; override with LINUX_FIRMWARE_REF=).  Point
# LINUX_FIRMWARE_SRC=/lib/firmware at a distro firmware tree to skip the git
# checkout for local runs; .zst/.xz members are decompressed during staging.
LINUX_FIRMWARE_REF ?= 20260910
build/firmware/.stamp: scripts/linux-firmware-install.sh scripts/linux/firmware-manifest.txt
	LINUX_FIRMWARE_REF='$(LINUX_FIRMWARE_REF)' scripts/linux-firmware-install.sh
	@touch $@

disk.img: userland/winoptions userland/icewm-menu $(ALPINE_STAMP)
disk.img: scripts/configure-accounts.sh userland/avory-account userland/test_accounts.sh userland/avory-login.elf userland/avory-installer.elf
disk.img:  userland/dns_lookup.elf userland/nettest.elf
disk.img: userland/test_clone_futex.elf
# Fixed 12 GiB image by default (override per build with
# `make disk.img DISK_SIZE_MIB=<MiB>`). Leave room for filesystem metadata
# and tools added after the rootfs overlay.
DISK_SIZE_MIB ?= 12288

disk.img: scripts/populate-ext2-dir.sh
# Keep the VFIO desktop profile even when disk.img is rebuilt independently
# of a run target. Non-passthrough images can use XORG_PROFILE=native.
XORG_PROFILE ?= amdgpu
XORG_CONFIG = $(if $(filter amdgpu,$(XORG_PROFILE)),config/xorg/10-modesetting.conf,config/xorg/10-native.conf)

# Pick a profile when populating a disk. Changing launch targets does not
# switch profiles; the default remains amdgpu for standalone builds too.
# To install the non-passthrough profile, rebuild with XORG_PROFILE=native.
disk.img: config/xorg/10-modesetting.conf config/xorg/10-native.conf config/xorg/20-output.conf scripts/image-runtime.py
disk.img: config/xorg/avory-prime-display.sh config/xorg/45-prime-output.sh
disk.img: scripts/image-firmware-install.sh
disk.img: userland/test_signal_stack.elf
disk.img: userland/test_user_dispatch.elf
disk.img: userland/test_syscompat.elf
disk.img: userland/test_tlb_stress.elf
disk.img: userland/test_futex_clock.elf
disk.img: userland/test_futex_pi.elf
disk.img: userland/test_unix_sockets.elf
disk.img: userland/test_syscall_speed.elf
disk.img: userland/test_mm_speed.elf
disk.img: userland/test_journal_speed.elf
disk.img: userland/test_syscall_cpu.elf
disk.img: userland/test_blocking_liveness.elf
disk.img: userland/test_hugepages.elf
disk.img: userland/test_zero_page.elf
disk.img: userland/test_copy_user.elf
disk.img: userland/test_pcp_pmm.elf
disk.img: userland/test_ticket_lock.elf
disk.img: userland/test_fb_perf.elf
disk.img: userland/test_vdso_bench.elf
disk.img: userland/test_lazy_fpu.elf
disk.img: userland/test_elf32.elf
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
disk.img: userland/test_kpi_amdgpu.elf
disk.img: userland/test_kpi_radeonsi.elf userland/radeonsi_vram_leak.elf
disk.img: build/test_fw.bin build/firmware/.stamp
disk.img: userland/butterscotch.elf assets/game.unx assets/assets


disk.img: $(BASH_STAMP) $(COREUTILS_STAMP) $(ALPINE_STAMP) $(QUAKE2_BUNDLE_FILES)
disk.img: assets/boot.wav userland/test.c assets/test.wav assets/jane.mp3 assets/mc9.mp3 assets/train.mp3 assets/test.bmp assets/test.tar assets/room.png assets/logo.png assets/linus.gif assets/video.mp4 userland/about.elf userland/hello_glibc.elf userland/booter.elf userland/reboot.elf userland/shutdown.elf userland/apm.elf userland/test_cpp.elf  userland/kilo.elf  userland/ls.elf userland/lspci.elf userland/lsblk.elf userland/readelf.elf userland/pong.elf userland/raycast.elf userland/asplay.elf userland/kria.elf userland/doom.elf userland/doom_x11.elf userland/gtk3_test.elf userland/qt5_test.elf userland/sdl3_test.elf userland/tglgears_fb.elf userland/tglgears_drm.elf userland/tglhello_drm.elf userland/test_mem_stress.elf userland/classicube.elf userland/terrain.png userland/texpacks/classicube.zip initrd/startx.sh initrd/startw.sh initrd/weston.ini initrd/drm-pick.sh initrd/avory-drm.sh AetherDE/x11-wm/AetherWM AetherDE/aether-dock/aether-dock AetherDE/aether-panel/aether-panel AetherDE/wayland-compositor/aether-compositor AetherDE/demo-client/aether-window AetherDE/scripts/sax11.sh AetherDE/scripts/sawayland.sh userland/avoryd.elf $(AVORYD_CONFIG_FILES)
	@echo "Creating partitioned disk image (GPT)..."
	@if ls -l /proc/[0-9]*/fd/* 2>/dev/null | grep -q "disk.img"; then \
		echo "Refusing: disk.img is open by a running process (QEMU?). Stop the VM before rebuilding the disk image; rebuilding it in place corrupts the guest's filesystem." >&2; \
		exit 1; \
	fi
	@if _rootfs_mb=$$(du -sm build/alpine/rootfs 2>/dev/null | cut -f1); \
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
	mkfs.ext4 -F -b 1024 -I 128 \
		-O extent,filetype,has_journal,dir_index,^64bit,^metadata_csum,^flex_bg,^huge_file,^dir_nlink,^extra_isize,^metadata_csum_seed,^orphan_file \
		./part.img
	@echo "Populating root filesystem..."
	@{ \
		echo "cd /"; \
		echo "mkdir tmp"; \
		echo "mkdir bin"; \
		echo "mkdir .config"; \
		echo "mkdir .cache"; \
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
	} | debugfs -w ./part.img >/dev/null 2>&1 || true
	@if [ -d AetherDE ]; then \
		echo "Installing AetherDE into disk image (/AetherDE)..."; \
		./scripts/populate-ext2-dir.sh ./part.img AetherDE AetherDE; \
	fi
	@if [ -d toolchain/glibc-sysroot/usr/include ]; then \
		echo "Installing GLIBC headers into disk image (opt/glibc/include)..."; \
		./scripts/populate-ext2-dir.sh ./part.img toolchain/glibc-sysroot/usr/include opt/glibc/include; \
	fi
	# NOTE: the toolchain glibc *libs* are deliberately NOT copied anymore.
	# The userland tree now ships its own glibc (Arch), and overlaying the
	# older toolchain build on top mixes two glibc versions in /usr/lib,
	# which breaks every dynamically linked binary (e.g. bash failing on
	# __pointer_chk_guard). This step only made sense for the old musl tree.
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
		echo "rm bin/radeonsi_vram_leak.elf"; \
		echo "write userland/radeonsi_vram_leak.elf bin/radeonsi_vram_leak.elf"; \
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
	@if [ -d build/alpine/rootfs ]; then \
		mkdir -p build/alpine/rootfs/lib/firmware; \
		cp -f build/test_fw.bin build/alpine/rootfs/lib/firmware/test_fw.bin; \
		if [ -d build/firmware/lib/firmware ]; then \
			cp -a build/firmware/lib/firmware/. build/alpine/rootfs/lib/firmware/; \
		fi; \
	fi
	@if [ -d build/alpine/rootfs ]; then \
		echo "Populating Arch Linux rootfs into disk image..."; \
		mkdir -p build/alpine/rootfs/etc/X11/xorg.conf.d; \
		install -m 0644 $(XORG_CONFIG) build/alpine/rootfs/etc/X11/xorg.conf.d/10-modesetting.conf; \
		install -m 0755 config/xorg/avory-prime-display.sh build/alpine/rootfs/usr/bin/avory-prime-display; \
		mkdir -p build/alpine/rootfs/etc/X11/xinit/xinitrc.d; \
		install -m 0755 config/xorg/45-prime-output.sh build/alpine/rootfs/etc/X11/xinit/xinitrc.d/45-prime-output.sh; \
		mkdir -p build/alpine/rootfs/etc/sddm.conf.d; \
		install -m 0644 config/xorg/20-output.conf build/alpine/rootfs/etc/sddm.conf.d/20-output.conf; \
		if [ -f build/alpine/rootfs/etc/sddm.conf ]; then \
			sed -i 's|^DisplayCommand=.*$$|DisplayCommand=/usr/bin/avory-prime-display|' build/alpine/rootfs/etc/sddm.conf; \
		fi; \
		./scripts/configure-accounts.sh build/alpine/rootfs; \
		./scripts/populate-ext2-dir.sh ./part.img build/alpine/rootfs /; \
	fi
	@python3 scripts/image-runtime.py ./part.img build/alpine/rootfs \
		/usr/lib/libc.so.6 /usr/lib/ld-linux-x86-64.so.2
	# The Arch rootfs also has a stale bin/startx.sh.  Restore these
	# AvoryOS-owned files after the overlay, otherwise XFCE runs startxfce4
	# and opens its failsafe-session error dialog.
	@{ \
		echo "cd /"; \
		echo "rm bin/startx.sh"; \
		echo "write initrd/startx.sh bin/startx.sh"; \
		echo "rm bin/startw.sh"; \
		echo "write initrd/startw.sh bin/startw.sh"; \
		echo "rm bin/drm-pick.sh"; \
		echo "write initrd/drm-pick.sh bin/drm-pick.sh"; \
		echo "rm etc/profile.d/avory-drm.sh"; \
		echo "write initrd/avory-drm.sh etc/profile.d/avory-drm.sh"; \
		echo "rm etc/weston.ini"; \
		echo "write initrd/weston.ini etc/weston.ini"; \
		echo "set_inode_field bin/startx.sh mode 0100755"; \
		echo "set_inode_field bin/startw.sh mode 0100755"; \
		echo "set_inode_field bin/drm-pick.sh mode 0100755"; \
		echo "set_inode_field etc/profile.d/avory-drm.sh mode 0100644"; \
		echo "set_inode_field var/lib/sddm mode 040775"; \
		_sddm_u=$$(awk -F: '/^sddm:/{print $$3; exit}' build/alpine/rootfs/etc/passwd 2>/dev/null); \
		_sddm_g=$$(awk -F: '/^sddm:/{print $$3; exit}' build/alpine/rootfs/etc/group 2>/dev/null); \
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
	@echo "Installing staged glibc into /lib and /lib64 (no symlinks)..."
	@# The guest kernel does not resolve the usrmerge symlinks for the
	@# interpreter path (/lib64/ld-linux... -> usr/lib/...): with /lib64 as
	@# a symlink, exec of /bin/sh fails with
	@#   ELF load failed: File not found: /lib64/ld-linux-x86-64.so.2
	@# while a real /lib64 dir with the old toolchain glibc 2.43 loads but
	@# then fails every dynamic binary with
	@#   undefined symbol: __pointer_chk_guard, version GLIBC_PRIVATE
	@# (Arch glibc is 2.44: libc UNDs it, ld.so defines it; 2.43 has neither).
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
	@python3 scripts/image-runtime.py ./part.img build/alpine/rootfs --copy-compat
	@bash scripts/image-firmware-install.sh ./part.img
	@echo "Populating root filesystem with additional tools..."

	@if [ -d build/tcc-glibc-install/opt/tcc ]; then \
		echo "Installing GLIBC TCC into disk image..."; \
		./scripts/populate-ext2-dir.sh ./part.img build/tcc-glibc-install/opt/tcc opt/tcc; \
		debugfs -w -R "rm bin/tcc" ./part.img >/dev/null 2>&1 || true; \
		debugfs -w -R "write build/tcc-glibc-install/opt/tcc/bin/tcc bin/tcc" ./part.img >/dev/null 2>&1 || true; \
		debugfs -w -R "rm usr/lib/libtcc.so" ./part.img >/dev/null 2>&1 || true; \
		debugfs -w -R "write build/tcc-glibc-install/opt/tcc/lib/libtcc.so usr/lib/libtcc.so" ./part.img >/dev/null 2>&1 || true; \
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
		echo 'export XDG_RUNTIME_DIR="$${XDG_RUNTIME_DIR:-/tmp/runtime-$$(id -u)}"' >> /tmp/bashrc; \
		echo 'mkdir -p "$$XDG_RUNTIME_DIR" 2>/dev/null && chmod 1777 "$$XDG_RUNTIME_DIR" 2>/dev/null' >> /tmp/bashrc; \
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
		echo '{"general": {"detectVersion": false}, "logo": {"source": "/fastfetch/logo.txt", "type": "auto"}, "modules": ["title", "separator", "os", "kernel", "uptime", "packages", {"type": "shell", "format": "bash"}, "de", "wm", "terminal", "cursor", "cpu", "gpu", "memory", "swap", "disk", {"type": "localip", "key": "Local IP", "showIpv6": false}, "locale", "break", "colors"]}' > /tmp/ff_config.jsonc; \
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
		echo "set_inode_field bin/avory-login mode 0100755"; \
		echo "set_inode_field bin/avory-installer mode 0100755"; \
		echo "set_inode_field usr/sbin/avory-installer mode 0100755"; \
		echo "set_inode_field bin/avoryd mode 0100755"; \
		echo "set_inode_field bin/sax11.sh mode 0100755"; \
		echo "set_inode_field bin/sawayland.sh mode 0100755"; \
		echo "set_inode_field bin/AetherWM mode 0100755"; \
		echo "set_inode_field bin/aether-dock mode 0100755"; \
		echo "set_inode_field bin/aether-panel mode 0100755"; \
		echo "set_inode_field bin/aether-compositor mode 0100755"; \
		echo "set_inode_field bin/aether-window mode 0100755"; \
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

	@if [ -L ./part.img ]; then \
		loop=$$(readlink ./part.img); \
		rm -f ./part.img; \
		losetup -d "$$loop"; \
	else \
		dd if=./part.img of=disk.img bs=1M seek=1 conv=notrunc,sparse; \
		rm -f ./part.img; \
	fi
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

# Optional guest kernel command line baked into the ISO (limine `cmdline:`).
# linuxkpi_param_parse() applies kpi_* module parameters from it, e.g.:
#   make run-vfio KERNEL_CMDLINE=kpi_vfio_test=1
KERNEL_CMDLINE ?=

# Make tracks files, not variable values: without this stamp a new
# KERNEL_CMDLINE would silently reuse the previously baked ISO cmdline.
.PHONY: FORCE
FORCE:

build/kernel_cmdline.stamp: FORCE
	@mkdir -p build
	@printf '%s' '$(KERNEL_CMDLINE)' > $@.tmp
	@if cmp -s $@.tmp $@; then rm -f $@.tmp; else mv $@.tmp $@; fi

$(IMAGE_NAME).iso: limine/limine kernel limine.conf build/kernel_cmdline.stamp
	rm -rf iso_root
	mkdir -p iso_root/boot
	cp -v kernel/bin-$(ARCH)/kernel iso_root/boot/
	# Ship a stripped copy.  The build ELF carries ~95 MB of debug info that
	# addr2line/GDB use against kernel/bin-$(ARCH)/kernel; nothing at runtime
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
	rm -rf iso_root

.PHONY: clean
clean:
	$(MAKE) -C kernel clean
	rm -f $(IMAGE_NAME).iso

.PHONY: clean-all
clean-all: clean-musl clean-doom clean-coreutils clean-tar clean-apm clean-minimal
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

userland/avory-installer.elf: userland/avory-installer.c $(MUSL_LIBC)
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" $(MUSL_CC) $(MUSL_USER_CFLAGS) \
		userland/avory-installer.c -o userland/avory-installer.elf

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

userland/test_signal_stack.elf: userland/test_signal_stack.c $(MUSL_LIBC)
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" $(MUSL_CC) $(MUSL_USER_CFLAGS) \
		userland/test_signal_stack.c -o userland/test_signal_stack.elf -lpthread

userland/test_user_dispatch.elf: userland/test_user_dispatch.c $(MUSL_LIBC)
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" $(MUSL_CC) $(MUSL_USER_CFLAGS) \
		userland/test_user_dispatch.c -o userland/test_user_dispatch.elf

userland/test_futex_clock.elf: userland/test_futex_clock.c $(MUSL_LIBC)
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" $(MUSL_CC) $(MUSL_USER_CFLAGS) \
		userland/test_futex_clock.c -o userland/test_futex_clock.elf

userland/test_tlb_stress.elf: userland/test_tlb_stress.c $(MUSL_LIBC)
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" $(MUSL_CC) $(MUSL_USER_CFLAGS) \
		userland/test_tlb_stress.c -o userland/test_tlb_stress.elf -lpthread

userland/test_syscompat.elf: userland/test_syscompat.c $(MUSL_LIBC)
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" $(MUSL_CC) $(MUSL_USER_CFLAGS) \
		userland/test_syscompat.c -o userland/test_syscompat.elf

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

# Native syscall restart regression using interruptible LinuxKPI fence waits.
userland/test_syscall_restart.elf: userland/test_syscall_restart.c $(MUSL_LIBC) \
		kernel/linuxkpi/include/uapi/kpi_dmabuf.h
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" $(MUSL_CC) $(MUSL_USER_CFLAGS) \
		-I$(CURDIR)/kernel/linuxkpi/include \
		userland/test_syscall_restart.c -o userland/test_syscall_restart.elf

# Phase 6 C5 amdgpu test: raw-ioctl GEM/PRIME/CTX/VM/BO_LIST, an SDMA copy
# through AMDGPU_CS + WAIT_CS, and a 1k BO churn loop with a PMM invariant.
userland/test_kpi_amdgpu.elf: userland/test_kpi_amdgpu.c $(MUSL_LIBC) \
		kernel/linuxkpi/include/uapi/kpi_dmabuf.h
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" $(MUSL_CC) $(MUSL_USER_CFLAGS) \
		-I$(CURDIR)/kernel/linuxkpi/include \
		userland/test_kpi_amdgpu.c -o userland/test_kpi_amdgpu.elf

# Phase 6 C7 radeonsi test: headless GBM/EGL/GLES on amdgpu's render node.
# Unlike the raw-ioctl suites this one must be dynamically linked: it uses the
# rootfs Mesa/GBM/EGL shared libraries (static Mesa is not packaged),
# so it uses the guest glibc loader. It asserts the renderer is radeonsi,
# renders timed frames (fps) and round-trips a dma-buf through EGL.
userland/test_kpi_radeonsi.elf: userland/test_kpi_radeonsi.c $(GLIBC_STAMP) \
		$(ALPINE_SYSROOT)/usr/lib/libEGL.so
	$(GLIBC_CC) $(GLIBC_USER_CFLAGS) \
		-I$(ALPINE_SYSROOT)/usr/include \
		-I$(ALPINE_SYSROOT)/usr/include/libdrm \
		userland/test_kpi_radeonsi.c -o userland/test_kpi_radeonsi.elf \
		-L$(GLIBC_SYSROOT)/lib -L$(ALPINE_SYSROOT)/usr/lib -L$(ALPINE_SYSROOT)/lib \
		-lEGL -lgbm -lGLESv2 -ldrm -lm \
		-Wl,-dynamic-linker,/lib64/ld-linux-x86-64.so.2 \
		-Wl,-rpath,/usr/lib \
		-Wl,-rpath-link,$(ALPINE_SYSROOT)/usr/lib:$(ALPINE_SYSROOT)/lib \
		-Wl,--allow-shlib-undefined

# Bounded memory-lifetime diagnostic using the guest Mesa runtime.
userland/radeonsi_vram_leak.elf: userland/radeonsi_vram_leak.c $(GLIBC_STAMP) \
		$(ALPINE_SYSROOT)/usr/lib/libEGL.so
	$(GLIBC_CC) $(GLIBC_USER_CFLAGS) \
		-I$(ALPINE_SYSROOT)/usr/include \
		-I$(ALPINE_SYSROOT)/usr/include/libdrm \
		$< -o $@ \
		-L$(GLIBC_SYSROOT)/lib -L$(ALPINE_SYSROOT)/usr/lib -L$(ALPINE_SYSROOT)/lib \
		-lEGL -lgbm -lGLESv2 -ldrm \
		-Wl,-dynamic-linker,/lib64/ld-linux-x86-64.so.2 \
		-Wl,-rpath,/usr/lib \
		-Wl,-rpath-link,$(ALPINE_SYSROOT)/usr/lib:$(ALPINE_SYSROOT)/lib \
		-Wl,--allow-shlib-undefined

# Offscreen asynchronous shader-constant oracle. Uses the original guest Mesa
# runtime; building this diagnostic neither rebuilds Mesa nor updates disk.img.
userland/test_gpu_constants.elf: userland/test_gpu_constants.c $(GLIBC_STAMP) \
		$(ALPINE_SYSROOT)/usr/lib/libEGL.so
	$(GLIBC_CC) $(GLIBC_USER_CFLAGS) \
		-I$(ALPINE_SYSROOT)/usr/include \
		-I$(ALPINE_SYSROOT)/usr/include/libdrm \
		userland/test_gpu_constants.c -o $@ \
		-L$(GLIBC_SYSROOT)/lib -L$(ALPINE_SYSROOT)/usr/lib -L$(ALPINE_SYSROOT)/lib \
		-lEGL -lgbm -lGLESv2 -ldrm -lm \
		-Wl,-dynamic-linker,/lib64/ld-linux-x86-64.so.2 \
		-Wl,-rpath,/usr/lib \
		-Wl,-rpath-link,$(ALPINE_SYSROOT)/usr/lib:$(ALPINE_SYSROOT)/lib \
		-Wl,--allow-shlib-undefined

userland/test_gpu_pipeline.elf: userland/test_gpu_pipeline.c $(GLIBC_STAMP) \
		$(ALPINE_SYSROOT)/usr/lib/libEGL.so
	$(GLIBC_CC) $(GLIBC_USER_CFLAGS) \
		-I$(ALPINE_SYSROOT)/usr/include \
		-I$(ALPINE_SYSROOT)/usr/include/libdrm \
		userland/test_gpu_pipeline.c -o $@ \
		-L$(GLIBC_SYSROOT)/lib -L$(ALPINE_SYSROOT)/usr/lib -L$(ALPINE_SYSROOT)/lib \
		-lEGL -lgbm -lGLESv2 -ldrm -lm \
		-Wl,-dynamic-linker,/lib64/ld-linux-x86-64.so.2 \
		-Wl,-rpath,/usr/lib \
		-Wl,-rpath-link,$(ALPINE_SYSROOT)/usr/lib:$(ALPINE_SYSROOT)/lib \
		-Wl,--allow-shlib-undefined

# Same CPU oracles through desktop OpenGL; GLES controls remain available.
userland/test_gpu_pipeline_gl.elf: userland/test_gpu_pipeline.c $(GLIBC_STAMP) \
		$(ALPINE_SYSROOT)/usr/lib/libEGL.so
	$(GLIBC_CC) $(GLIBC_USER_CFLAGS) -DPIPELINE_DESKTOP \
		-I$(ALPINE_SYSROOT)/usr/include \
		-I$(ALPINE_SYSROOT)/usr/include/libdrm \
		userland/test_gpu_pipeline.c -o $@ \
		-L$(GLIBC_SYSROOT)/lib -L$(ALPINE_SYSROOT)/usr/lib -L$(ALPINE_SYSROOT)/lib \
		-lEGL -lgbm -lGL -ldrm -lm \
		-Wl,-dynamic-linker,/lib64/ld-linux-x86-64.so.2 \
		-Wl,-rpath,/usr/lib \
		-Wl,-rpath-link,$(ALPINE_SYSROOT)/usr/lib:$(ALPINE_SYSROOT)/lib \
		-Wl,--allow-shlib-undefined

# Fog-independent framebuffer oracle with bounded PNG mismatch captures.
userland/test_gpu_corruption.elf: userland/test_gpu_corruption.c $(GLIBC_STAMP) \
		$(ALPINE_SYSROOT)/usr/lib/libEGL.so
	$(GLIBC_CC) $(GLIBC_USER_CFLAGS) \
		-I$(ALPINE_SYSROOT)/usr/include \
		-I$(ALPINE_SYSROOT)/usr/include/libdrm \
		userland/test_gpu_corruption.c -o $@ \
		-L$(GLIBC_SYSROOT)/lib -L$(ALPINE_SYSROOT)/usr/lib -L$(ALPINE_SYSROOT)/lib \
		-lEGL -lgbm -lGL -ldrm -lz \
		-Wl,-dynamic-linker,/lib64/ld-linux-x86-64.so.2 \
		-Wl,-rpath,/usr/lib \
		-Wl,-rpath-link,$(ALPINE_SYSROOT)/usr/lib:$(ALPINE_SYSROOT)/lib \
		-Wl,--allow-shlib-undefined

# Depth/stencil/reuse/MSAA oracle independent of presentation.
userland/test_gpu_depth.elf: userland/test_gpu_depth.c $(GLIBC_STAMP) \
		$(ALPINE_SYSROOT)/usr/lib/libEGL.so
	$(GLIBC_CC) $(GLIBC_USER_CFLAGS) \
		-I$(ALPINE_SYSROOT)/usr/include \
		-I$(ALPINE_SYSROOT)/usr/include/libdrm \
		userland/test_gpu_depth.c -o $@ \
		-L$(GLIBC_SYSROOT)/lib -L$(ALPINE_SYSROOT)/usr/lib -L$(ALPINE_SYSROOT)/lib \
		-lEGL -lgbm -lGL -ldrm -lm \
		-Wl,-dynamic-linker,/lib64/ld-linux-x86-64.so.2 \
		-Wl,-rpath,/usr/lib \
		-Wl,-rpath-link,$(ALPINE_SYSROOT)/usr/lib:$(ALPINE_SYSROOT)/lib \
		-Wl,--allow-shlib-undefined

.PHONY: gpu-diagnostics
gpu-diagnostics: userland/test_gpu_corruption.elf userland/test_gpu_depth.elf userland/test_gpu_constants.elf userland/test_gpu_pipeline.elf userland/test_gpu_pipeline_gl.elf userland/radeonsi_vram_leak.elf
	mkdir -p build/gpu-diagnostics
	install -m 0755 userland/test_gpu_corruption.elf build/gpu-diagnostics/test_gpu_corruption
	install -m 0755 userland/test_gpu_depth.elf build/gpu-diagnostics/test_gpu_depth
	install -m 0755 userland/test_gpu_constants.elf build/gpu-diagnostics/test_gpu_constants
	install -m 0755 userland/test_gpu_pipeline.elf build/gpu-diagnostics/test_gpu_pipeline
	install -m 0755 userland/test_gpu_pipeline_gl.elf build/gpu-diagnostics/test_gpu_pipeline_gl
	install -m 0755 userland/radeonsi_vram_leak.elf build/gpu-diagnostics/radeonsi_vram_leak.elf
	install -m 0755 scripts/gpu-debug-capture.sh build/gpu-diagnostics/gpu-debug-capture
	@echo 'Diagnostics ready. Serve to the QEMU guest with:'
	@echo 'python3 -m http.server 9007 --bind 127.0.0.1 --directory build/gpu-diagnostics'

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

userland/test_mm_speed.elf: userland/test_mm_speed.c $(MUSL_LIBC)
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" $(MUSL_CC) $(MUSL_USER_CFLAGS) \
		userland/test_mm_speed.c -o userland/test_mm_speed.elf

userland/test_journal_speed.elf: userland/test_journal_speed.c $(MUSL_LIBC)
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" $(MUSL_CC) $(MUSL_USER_CFLAGS) \
		userland/test_journal_speed.c -o userland/test_journal_speed.elf -lpthread

userland/test_syscall_cpu.elf: userland/test_syscall_cpu.c $(MUSL_LIBC)
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" $(MUSL_CC) $(MUSL_USER_CFLAGS) \
		userland/test_syscall_cpu.c -o userland/test_syscall_cpu.elf

userland/test_blocking_liveness.elf: userland/test_blocking_liveness.c $(MUSL_LIBC)
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" $(MUSL_CC) $(MUSL_USER_CFLAGS) \
		userland/test_blocking_liveness.c -o userland/test_blocking_liveness.elf -lpthread

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

userland/test_elf32.elf: userland/test_elf32.asm
	mkdir -p build
	nasm -f elf32 $< -o build/test_elf32.o
	ld -m elf_i386 -static -z max-page-size=0x1000 -o $@ build/test_elf32.o

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
