# Userland application builds. Run from the repository root with `make -C userland`.
.DEFAULT_GOAL := all
ROOT_DIR := $(abspath ..)
GLIBC_TOOLCHAIN_BIN ?= $(ROOT_DIR)/toolchain/x86_64-linux-glibc/bin
GLIBC_TOOLCHAIN_STAMP ?= $(GLIBC_TOOLCHAIN_BIN)/x86_64-linux-gcc
GLIBC_SYSROOT ?= $(ROOT_DIR)/build/arch/rootfs
GLIBC_STAMP ?= $(ROOT_DIR)/build/arch/.built
GLIBC_CC ?= $(GLIBC_TOOLCHAIN_BIN)/x86_64-linux-gcc
GLIBC_CXX ?= $(GLIBC_TOOLCHAIN_BIN)/x86_64-linux-g++
GLIBC_USER_CFLAGS ?= -O2 -Wall -Wextra -fno-stack-protector --sysroot=$(GLIBC_SYSROOT) \
	-Wl,-T,$(ROOT_DIR)/config/userland-link.ld
ARCH_SYSROOT ?= $(ROOT_DIR)/build/arch/rootfs
ARCH_STAMP ?= $(ROOT_DIR)/build/arch/.built

$(GLIBC_TOOLCHAIN_STAMP):
	$(MAKE) -C $(ROOT_DIR) glibc-toolchain

$(GLIBC_STAMP): $(GLIBC_TOOLCHAIN_STAMP)
	$(MAKE) -C $(ROOT_DIR) setup-rootfs

.PHONY: all gpu-diagnostics
all: avory-login.elf avory-installer.elf hello_glibc.elf booter.elf reboot.elf shutdown.elf kilo.elf ls.elf lspci.elf lsblk.elf readelf.elf pong.elf raycast.elf asplay.elf dns_lookup.elf kria.elf drm_identify.elf

avory-login.elf: avory-login.c $(GLIBC_STAMP)
	$(GLIBC_CC) $(GLIBC_USER_CFLAGS) \
		avory-login.c -lcrypt -o avory-login.elf

avory-installer.elf: avory-installer.c $(GLIBC_STAMP)
	$(GLIBC_CC) $(GLIBC_USER_CFLAGS) \
		avory-installer.c -o avory-installer.elf

nvme_init.elf: nvme_init.c $(GLIBC_STAMP)
	$(GLIBC_CC) $(GLIBC_USER_CFLAGS) -static nvme_init.c -o nvme_init.elf

nvme_test.elf: nvme_test.c $(GLIBC_STAMP)
	$(GLIBC_CC) $(GLIBC_USER_CFLAGS) \
		nvme_test.c -o nvme_test.elf -lpthread -lm

nvme_bench.elf: nvme_bench.c $(GLIBC_STAMP)
	$(GLIBC_CC) $(GLIBC_USER_CFLAGS) \
		nvme_bench.c -o nvme_bench.elf -lpthread

hello_glibc.elf: hello_glibc.c $(GLIBC_STAMP)
	$(GLIBC_CC) $(GLIBC_USER_CFLAGS) \
		hello_glibc.c -o hello_glibc.elf

kilo.c:
	curl -L https://raw.githubusercontent.com/antirez/kilo/master/kilo.c -o kilo.c

kilo.elf: kilo.c $(GLIBC_STAMP)
	$(GLIBC_CC) $(GLIBC_USER_CFLAGS) \
		kilo.c -o kilo.elf

ls.elf: ls.c $(GLIBC_STAMP)
	$(GLIBC_CC) $(GLIBC_USER_CFLAGS) \
		ls.c -o ls.elf

lspci.elf: lspci.c $(GLIBC_STAMP)
	$(GLIBC_CC) $(GLIBC_USER_CFLAGS) \
		lspci.c -o lspci.elf

lsblk.elf: lsblk.c $(GLIBC_STAMP)
	$(GLIBC_CC) $(GLIBC_USER_CFLAGS) \
		lsblk.c -o lsblk.elf

readelf.elf: readelf.c $(GLIBC_STAMP)
	$(GLIBC_CC) $(GLIBC_USER_CFLAGS) \
		readelf.c -o readelf.elf

pong.elf: pong.c $(GLIBC_STAMP)
	$(GLIBC_CC) $(GLIBC_USER_CFLAGS) \
		pong.c -o pong.elf \
		-lm

raycast.elf: raycast.c $(GLIBC_STAMP)
	$(GLIBC_CC) $(GLIBC_USER_CFLAGS) \
		raycast.c -lm -o raycast.elf

asplay.elf: asplay.c $(GLIBC_STAMP)
	$(GLIBC_CC) $(GLIBC_USER_CFLAGS) \
		asplay.c -o asplay.elf

booter.elf: booter.c $(GLIBC_STAMP)
	$(GLIBC_CC) $(GLIBC_USER_CFLAGS) \
		booter.c -o booter.elf

reboot.elf: reboot.c $(GLIBC_STAMP)
	$(GLIBC_CC) $(GLIBC_USER_CFLAGS) \
		reboot.c -o reboot.elf

shutdown.elf: shutdown.c $(GLIBC_STAMP)
	$(GLIBC_CC) $(GLIBC_USER_CFLAGS) \
		shutdown.c -o shutdown.elf

dns_lookup.elf: dns_lookup.c dns_resolver.c dns_resolver.h $(GLIBC_STAMP)
	$(GLIBC_CC) $(GLIBC_USER_CFLAGS) \
		dns_lookup.c dns_resolver.c -o dns_lookup.elf

# Kria programming language (Rust-based, compiled with musl for static linking)
kria-lang:
	rm -rf kria-lang
	git clone https://github.com/Piotriox/kria-lang.git kria-lang
	mkdir -p kria-lang/.cargo
	echo '[build]' > kria-lang/.cargo/config.toml
	echo 'target = "x86_64-unknown-linux-musl"' >> kria-lang/.cargo/config.toml

kria.elf: kria-lang
	cd kria-lang && cargo build --release
	cp kria-lang/target/x86_64-unknown-linux-musl/release/kria kria.elf

.PHONY: kria
kria: kria.elf

test_mem_stress.elf: test_mem_stress.c $(GLIBC_STAMP)
	$(GLIBC_CC) $(GLIBC_USER_CFLAGS) \
		test_mem_stress.c -o test_mem_stress.elf

test_clone_futex.elf: test_clone_futex.c test_clone_futex_trampoline.S $(GLIBC_STAMP)
	$(GLIBC_CC) $(GLIBC_USER_CFLAGS) \
		test_clone_futex.c test_clone_futex_trampoline.S -o test_clone_futex.elf

test_futex_pi.elf: test_futex_pi.c $(GLIBC_STAMP)
	$(GLIBC_CC) $(GLIBC_USER_CFLAGS) \
		test_futex_pi.c -o test_futex_pi.elf -lpthread -lm

test_signal_stack.elf: test_signal_stack.c $(GLIBC_STAMP)
	$(GLIBC_CC) $(GLIBC_USER_CFLAGS) \
		test_signal_stack.c -o test_signal_stack.elf -lpthread

test_user_dispatch.elf: test_user_dispatch.c $(GLIBC_STAMP)
	$(GLIBC_CC) $(GLIBC_USER_CFLAGS) \
		test_user_dispatch.c -o test_user_dispatch.elf

test_futex_clock.elf: test_futex_clock.c $(GLIBC_STAMP)
	$(GLIBC_CC) $(GLIBC_USER_CFLAGS) \
		test_futex_clock.c -o test_futex_clock.elf

test_tlb_stress.elf: test_tlb_stress.c $(GLIBC_STAMP)
	$(GLIBC_CC) $(GLIBC_USER_CFLAGS) \
		test_tlb_stress.c -o test_tlb_stress.elf -lpthread

test_syscompat.elf: test_syscompat.c $(GLIBC_STAMP)
	$(GLIBC_CC) $(GLIBC_USER_CFLAGS) \
		test_syscompat.c -o test_syscompat.elf

test_unix_sockets.elf: test_unix_sockets.c $(GLIBC_STAMP)
	$(GLIBC_CC) $(GLIBC_USER_CFLAGS) \
		test_unix_sockets.c -o test_unix_sockets.elf

test_tmpfile.elf: test_tmpfile.c $(GLIBC_STAMP)
	$(GLIBC_CC) $(GLIBC_USER_CFLAGS) \
		test_tmpfile.c -o test_tmpfile.elf

# Phase 2 LinuxKPI /dev/kpi_dmabuf test; shares the device ABI header with the
# kernel side (kernel/linuxkpi/include/uapi/kpi_dmabuf.h).
test_kpi_dmabuf.elf: test_kpi_dmabuf.c $(GLIBC_STAMP) \
		$(ROOT_DIR)/kernel/linuxkpi/include/uapi/kpi_dmabuf.h
	$(GLIBC_CC) $(GLIBC_USER_CFLAGS) \
		-I$(ROOT_DIR)/kernel/linuxkpi/include \
		test_kpi_dmabuf.c -o test_kpi_dmabuf.elf -lpthread

# Native syscall restart regression using interruptible LinuxKPI fence waits.
test_syscall_restart.elf: test_syscall_restart.c $(GLIBC_STAMP) \
		$(ROOT_DIR)/kernel/linuxkpi/include/uapi/kpi_dmabuf.h
	$(GLIBC_CC) $(GLIBC_USER_CFLAGS) \
		-I$(ROOT_DIR)/kernel/linuxkpi/include \
		test_syscall_restart.c -o test_syscall_restart.elf

# Phase 6 C5 amdgpu test: raw-ioctl GEM/PRIME/CTX/VM/BO_LIST, an SDMA copy
# through AMDGPU_CS + WAIT_CS, and a 1k BO churn loop with a PMM invariant.
test_kpi_amdgpu.elf: test_kpi_amdgpu.c $(GLIBC_STAMP) \
		$(ROOT_DIR)/kernel/linuxkpi/include/uapi/kpi_dmabuf.h
	$(GLIBC_CC) $(GLIBC_USER_CFLAGS) \
		-I$(ROOT_DIR)/kernel/linuxkpi/include \
		test_kpi_amdgpu.c -o test_kpi_amdgpu.elf

# Phase 6 C7 radeonsi test: headless GBM/EGL/GLES on amdgpu's render node.
# Unlike the raw-ioctl suites this one must be dynamically linked: it uses the
# rootfs Mesa/GBM/EGL shared libraries (static Mesa is not packaged),
# so it uses the guest glibc loader. It asserts the renderer is radeonsi,
# renders timed frames (fps) and round-trips a dma-buf through EGL.
test_kpi_radeonsi.elf: test_kpi_radeonsi.c $(GLIBC_STAMP) \
		$(ARCH_SYSROOT)/usr/lib/libEGL.so
	$(GLIBC_CC) $(GLIBC_USER_CFLAGS) \
		-I$(ARCH_SYSROOT)/usr/include \
		-I$(ARCH_SYSROOT)/usr/include/libdrm \
		test_kpi_radeonsi.c -o test_kpi_radeonsi.elf \
		-L$(GLIBC_SYSROOT)/lib -L$(ARCH_SYSROOT)/usr/lib -L$(ARCH_SYSROOT)/lib \
		-lEGL -lgbm -lGLESv2 -ldrm -lm \
		-Wl,-dynamic-linker,/lib64/ld-linux-x86-64.so.2 \
		-Wl,-rpath,/usr/lib \
		-Wl,-rpath-link,$(ARCH_SYSROOT)/usr/lib:$(ARCH_SYSROOT)/lib \
		-Wl,--allow-shlib-undefined

# Bounded memory-lifetime diagnostic using the guest Mesa runtime.
radeonsi_vram_leak.elf: radeonsi_vram_leak.c $(GLIBC_STAMP) \
		$(ARCH_SYSROOT)/usr/lib/libEGL.so
	$(GLIBC_CC) $(GLIBC_USER_CFLAGS) \
		-I$(ARCH_SYSROOT)/usr/include \
		-I$(ARCH_SYSROOT)/usr/include/libdrm \
		$< -o $@ \
		-L$(GLIBC_SYSROOT)/lib -L$(ARCH_SYSROOT)/usr/lib -L$(ARCH_SYSROOT)/lib \
		-lEGL -lgbm -lGLESv2 -ldrm \
		-Wl,-dynamic-linker,/lib64/ld-linux-x86-64.so.2 \
		-Wl,-rpath,/usr/lib \
		-Wl,-rpath-link,$(ARCH_SYSROOT)/usr/lib:$(ARCH_SYSROOT)/lib \
		-Wl,--allow-shlib-undefined

# Offscreen asynchronous shader-constant oracle. Uses the original guest Mesa
# runtime; building this diagnostic neither rebuilds Mesa nor updates disk.img.
test_gpu_constants.elf: test_gpu_constants.c $(GLIBC_STAMP) \
		$(ARCH_SYSROOT)/usr/lib/libEGL.so
	$(GLIBC_CC) $(GLIBC_USER_CFLAGS) \
		-I$(ARCH_SYSROOT)/usr/include \
		-I$(ARCH_SYSROOT)/usr/include/libdrm \
		test_gpu_constants.c -o $@ \
		-L$(GLIBC_SYSROOT)/lib -L$(ARCH_SYSROOT)/usr/lib -L$(ARCH_SYSROOT)/lib \
		-lEGL -lgbm -lGLESv2 -ldrm -lm \
		-Wl,-dynamic-linker,/lib64/ld-linux-x86-64.so.2 \
		-Wl,-rpath,/usr/lib \
		-Wl,-rpath-link,$(ARCH_SYSROOT)/usr/lib:$(ARCH_SYSROOT)/lib \
		-Wl,--allow-shlib-undefined

test_gpu_pipeline.elf: test_gpu_pipeline.c $(GLIBC_STAMP) \
		$(ARCH_SYSROOT)/usr/lib/libEGL.so
	$(GLIBC_CC) $(GLIBC_USER_CFLAGS) \
		-I$(ARCH_SYSROOT)/usr/include \
		-I$(ARCH_SYSROOT)/usr/include/libdrm \
		test_gpu_pipeline.c -o $@ \
		-L$(GLIBC_SYSROOT)/lib -L$(ARCH_SYSROOT)/usr/lib -L$(ARCH_SYSROOT)/lib \
		-lEGL -lgbm -lGLESv2 -ldrm -lm \
		-Wl,-dynamic-linker,/lib64/ld-linux-x86-64.so.2 \
		-Wl,-rpath,/usr/lib \
		-Wl,-rpath-link,$(ARCH_SYSROOT)/usr/lib:$(ARCH_SYSROOT)/lib \
		-Wl,--allow-shlib-undefined

# Same CPU oracles through desktop OpenGL; GLES controls remain available.
test_gpu_pipeline_gl.elf: test_gpu_pipeline.c $(GLIBC_STAMP) \
		$(ARCH_SYSROOT)/usr/lib/libEGL.so
	$(GLIBC_CC) $(GLIBC_USER_CFLAGS) -DPIPELINE_DESKTOP \
		-I$(ARCH_SYSROOT)/usr/include \
		-I$(ARCH_SYSROOT)/usr/include/libdrm \
		test_gpu_pipeline.c -o $@ \
		-L$(GLIBC_SYSROOT)/lib -L$(ARCH_SYSROOT)/usr/lib -L$(ARCH_SYSROOT)/lib \
		-lEGL -lgbm -lGL -ldrm -lm \
		-Wl,-dynamic-linker,/lib64/ld-linux-x86-64.so.2 \
		-Wl,-rpath,/usr/lib \
		-Wl,-rpath-link,$(ARCH_SYSROOT)/usr/lib:$(ARCH_SYSROOT)/lib \
		-Wl,--allow-shlib-undefined

# Fog-independent framebuffer oracle with bounded PNG mismatch captures.
test_gpu_corruption.elf: test_gpu_corruption.c $(GLIBC_STAMP) \
		$(ARCH_SYSROOT)/usr/lib/libEGL.so
	$(GLIBC_CC) $(GLIBC_USER_CFLAGS) \
		-I$(ARCH_SYSROOT)/usr/include \
		-I$(ARCH_SYSROOT)/usr/include/libdrm \
		test_gpu_corruption.c -o $@ \
		-L$(GLIBC_SYSROOT)/lib -L$(ARCH_SYSROOT)/usr/lib -L$(ARCH_SYSROOT)/lib \
		-lEGL -lgbm -lGL -ldrm -lz \
		-Wl,-dynamic-linker,/lib64/ld-linux-x86-64.so.2 \
		-Wl,-rpath,/usr/lib \
		-Wl,-rpath-link,$(ARCH_SYSROOT)/usr/lib:$(ARCH_SYSROOT)/lib \
		-Wl,--allow-shlib-undefined

# Depth/stencil/reuse/MSAA oracle independent of presentation.
test_gpu_depth.elf: test_gpu_depth.c $(GLIBC_STAMP) \
		$(ARCH_SYSROOT)/usr/lib/libEGL.so
	$(GLIBC_CC) $(GLIBC_USER_CFLAGS) \
		-I$(ARCH_SYSROOT)/usr/include \
		-I$(ARCH_SYSROOT)/usr/include/libdrm \
		test_gpu_depth.c -o $@ \
		-L$(GLIBC_SYSROOT)/lib -L$(ARCH_SYSROOT)/usr/lib -L$(ARCH_SYSROOT)/lib \
		-lEGL -lgbm -lGL -ldrm -lm \
		-Wl,-dynamic-linker,/lib64/ld-linux-x86-64.so.2 \
		-Wl,-rpath,/usr/lib \
		-Wl,-rpath-link,$(ARCH_SYSROOT)/usr/lib:$(ARCH_SYSROOT)/lib \
		-Wl,--allow-shlib-undefined

.PHONY: gpu-diagnostics
gpu-diagnostics: test_gpu_corruption.elf test_gpu_depth.elf test_gpu_constants.elf test_gpu_pipeline.elf test_gpu_pipeline_gl.elf radeonsi_vram_leak.elf
	mkdir -p $(ROOT_DIR)/build/gpu-diagnostics
	install -m 0755 test_gpu_corruption.elf $(ROOT_DIR)/build/gpu-diagnostics/test_gpu_corruption
	install -m 0755 test_gpu_depth.elf $(ROOT_DIR)/build/gpu-diagnostics/test_gpu_depth
	install -m 0755 test_gpu_constants.elf $(ROOT_DIR)/build/gpu-diagnostics/test_gpu_constants
	install -m 0755 test_gpu_pipeline.elf $(ROOT_DIR)/build/gpu-diagnostics/test_gpu_pipeline
	install -m 0755 test_gpu_pipeline_gl.elf $(ROOT_DIR)/build/gpu-diagnostics/test_gpu_pipeline_gl
	install -m 0755 radeonsi_vram_leak.elf $(ROOT_DIR)/build/gpu-diagnostics/radeonsi_vram_leak.elf
	install -m 0755 $(ROOT_DIR)/scripts/gpu-debug-capture.sh $(ROOT_DIR)/build/gpu-diagnostics/gpu-debug-capture
	install -m 0755 $(ROOT_DIR)/scripts/gpu-first-boot-capture.sh $(ROOT_DIR)/build/gpu-diagnostics/gpu-first-boot-capture.sh
	@echo 'Diagnostics ready. Serve to the QEMU guest with:'
	@echo 'python3 -m http.server 9007 --bind 127.0.0.1 --directory $(ROOT_DIR)/build/gpu-diagnostics'

# Phase 3 LinuxKPI DRM test: vkms dumb-buffer GEM mmap/write, renderD128
# sanity and a modetest-lite atomic enable/disable.
test_kpi_drm.elf: test_kpi_drm.c $(GLIBC_STAMP)
	$(GLIBC_CC) $(GLIBC_USER_CFLAGS) \
		test_kpi_drm.c -o test_kpi_drm.elf

drm_identify.elf: drm_identify.c $(GLIBC_STAMP)
	$(GLIBC_CC) $(GLIBC_USER_CFLAGS) -I$(ROOT_DIR)/kernel/linux/include/uapi/drm \
		drm_identify.c -o drm_identify.elf

# Phase 4 C5 bochs canary: card discovery by DRM version name, dumb-buffer
# mmap write/read and a full-CRTC atomic enable/disable.
test_kpi_bochs.elf: test_kpi_bochs.c $(GLIBC_STAMP)
	$(GLIBC_CC) $(GLIBC_USER_CFLAGS) \
		test_kpi_bochs.c -o test_kpi_bochs.elf

test_child_notify.elf: test_child_notify.c $(GLIBC_STAMP)
	$(GLIBC_CC) $(GLIBC_USER_CFLAGS) \
		test_child_notify.c -o test_child_notify.elf

test_pty_master.elf: test_pty_master.c $(GLIBC_STAMP)
	$(GLIBC_CC) $(GLIBC_USER_CFLAGS) \
		test_pty_master.c -o test_pty_master.elf

test_syscall_speed.elf: test_syscall_speed.c $(GLIBC_STAMP)
	$(GLIBC_CC) $(GLIBC_USER_CFLAGS) \
		test_syscall_speed.c -o test_syscall_speed.elf

test_mm_speed.elf: test_mm_speed.c $(GLIBC_STAMP)
	$(GLIBC_CC) $(GLIBC_USER_CFLAGS) \
		test_mm_speed.c -o test_mm_speed.elf

test_journal_speed.elf: test_journal_speed.c $(GLIBC_STAMP)
	$(GLIBC_CC) $(GLIBC_USER_CFLAGS) \
		test_journal_speed.c -o test_journal_speed.elf -lpthread

test_syscall_cpu.elf: test_syscall_cpu.c $(GLIBC_STAMP)
	$(GLIBC_CC) $(GLIBC_USER_CFLAGS) \
		test_syscall_cpu.c -o test_syscall_cpu.elf

test_blocking_liveness.elf: test_blocking_liveness.c $(GLIBC_STAMP)
	$(GLIBC_CC) $(GLIBC_USER_CFLAGS) \
		test_blocking_liveness.c -o test_blocking_liveness.elf -lpthread

test_hugepages.elf: test_hugepages.c $(GLIBC_STAMP)
	$(GLIBC_CC) $(GLIBC_USER_CFLAGS) \
		test_hugepages.c -o test_hugepages.elf

test_zero_page.elf: test_zero_page.c $(GLIBC_STAMP)
	$(GLIBC_CC) $(GLIBC_USER_CFLAGS) \
		test_zero_page.c -o test_zero_page.elf

test_watchdog.elf: test_watchdog.c $(GLIBC_STAMP)
	$(GLIBC_CC) $(GLIBC_USER_CFLAGS) \
		test_watchdog.c -o test_watchdog.elf

test_copy_user.elf: test_copy_user.c $(GLIBC_STAMP)
	$(GLIBC_CC) $(GLIBC_USER_CFLAGS) \
		test_copy_user.c -o test_copy_user.elf

test_pcp_pmm.elf: test_pcp_pmm.c $(GLIBC_STAMP)
	$(GLIBC_CC) $(GLIBC_USER_CFLAGS) \
		test_pcp_pmm.c -o test_pcp_pmm.elf -lpthread -lm

test_ticket_lock.elf: test_ticket_lock.c $(GLIBC_STAMP)
	$(GLIBC_CC) $(GLIBC_USER_CFLAGS) \
		test_ticket_lock.c -o test_ticket_lock.elf -lpthread -lm

test_fb_perf.elf: test_fb_perf.c $(GLIBC_STAMP)
	$(GLIBC_CC) $(GLIBC_USER_CFLAGS) \
		test_fb_perf.c -o test_fb_perf.elf -lpthread -lm

test_vdso_bench.elf: test_vdso_bench.c $(GLIBC_STAMP)
	$(GLIBC_CC) $(GLIBC_USER_CFLAGS) \
		test_vdso_bench.c -o test_vdso_bench.elf

test_elf32.elf: test_elf32.asm
	mkdir -p build
	nasm -f elf32 $< -o build/test_elf32.o
	ld -m elf_i386 -static -z max-page-size=0x1000 -o $@ build/test_elf32.o

test_lazy_fpu.elf: test_lazy_fpu.c $(GLIBC_STAMP)
	$(GLIBC_CC) $(GLIBC_USER_CFLAGS) \
		test_lazy_fpu.c -o test_lazy_fpu.elf -lpthread -lm

test_heap_smp.elf: test_heap_smp.c $(GLIBC_STAMP)
	$(GLIBC_CC) $(GLIBC_USER_CFLAGS) \
		test_heap_smp.c -o test_heap_smp.elf -lpthread -lm

test_dcache.elf: test_dcache.c $(GLIBC_STAMP)
	$(GLIBC_CC) $(GLIBC_USER_CFLAGS) \
		test_dcache.c -o test_dcache.elf

test_uaccess_bench.elf: test_uaccess_bench.c $(GLIBC_STAMP)
	$(GLIBC_CC) $(GLIBC_USER_CFLAGS) \
		test_uaccess_bench.c -o test_uaccess_bench.elf

test_readahead.elf: test_readahead.c $(GLIBC_STAMP)

	$(GLIBC_CC) $(GLIBC_USER_CFLAGS) \
		test_readahead.c -o test_readahead.elf

proc_bench.elf: proc_bench.c proc_bench_trampoline.S $(GLIBC_STAMP)



	$(GLIBC_CC) $(GLIBC_USER_CFLAGS) \
		proc_bench.c proc_bench_trampoline.S -o proc_bench.elf


panic_test.elf: panic_test.c $(GLIBC_STAMP)
	$(GLIBC_CC) $(GLIBC_USER_CFLAGS) \
		panic_test.c -o panic_test.elf

classicube.elf: $(ARCH_STAMP) $(GLIBC_STAMP) $(ROOT_DIR)/scripts/build-classicube.sh
	$(ROOT_DIR)/scripts/build-classicube.sh

texpacks/classicube.zip:
	@echo "ERROR: texpacks/classicube.zip not found."
	@echo "Please place the original ClassiCube texture pack zip at: texpacks/classicube.zip"
	@exit 1

terrain.png: texpacks/classicube.zip
	@echo "Extracting terrain.png from classicube.zip..."
	unzip -o texpacks/classicube.zip terrain.png

# Re-link C programs when their common linker compatibility settings change.
$(patsubst %.c,%.elf,$(wildcard *.c)): GNUmakefile $(ROOT_DIR)/config/userland-link.ld
