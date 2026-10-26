# Minecraft 1.16.5 rendering investigation

## GPU allocation limit

The original VM's `/logs/latest.log` records repeated `GL_OUT_OF_MEMORY` from
`glBufferData` and `glTexImage` at the time of the reported terrain holes.
The guest DRM file had only 4096 GEM slots, with slot zero unused. Creating
another live resource returned EMFILE despite available physical memory.
An unsuccessful buffer allocation can subsequently leave VirGL drawing with
an unbound vertex buffer. Removing the host guard is unsafe: it previously
prevented a reproduced NVIDIA host crash.

The per-client table now has 65536 slots. Zero and the legacy hardware
framebuffer handle 0xF0B0 are reserved. Duplicate-import scans stop at the
highest used slot, so opening an ordinary desktop client does not require
scanning the enlarged empty table on every allocation. Released slots remain
reusable, and final close releases references in bounded batches.

## Verification

- Source-backed registrar test: 6000 simultaneous objects, repeated imports,
  freed-slot reuse, reserved framebuffer handle. The previous capacity fails
  at live allocation 4096.
- Actual VirGL VM test: the previous kernel fails resource creation with
  errno 24 (EMFILE); the updated kernel passes 6000 simultaneous resources,
  texture roundtrip, queued submissions, fences, PRIME and final cleanup.
- Baseline Minecraft comparison: the previous kernel reproduced repeated
  GL_OUT_OF_MEMORY in glBufferData, then hit the host missing-buffer guard
  during an indexed terrain draw (2724 indices). A second run measured exactly
  4095 live handles despite 1107 MiB free RAM and logged 423 GL allocation errors.
- Private Minecraft copy: KVM, eight vCPUs, 4 GB guest RAM, KWin/Xwayland,
  Minecraft 1.16.5 vanilla, fancy graphics, 32-chunk stress distance, 2 GB Java
  heap, RTX 4060 Ti host. Kernel inspection measured 5904 live resources with
  852 MiB physical memory still free. The elevated landscape rendered without
  the reported holes, and this run's game log contained no GL allocation errors.
  This stress scene is heavier than the user's original 12-chunk settings;
  its FPS is not a comparison with the original 100 FPS report.

The test VMs used reflink copies of `disk.img`. During final packaging, the
normal ISO target unexpectedly entered its disk rebuild prerequisite. That
build was stopped; the main disk was recovered from the latest private copy.
The pre-test player metadata restored inventory, position and abilities, and
normal survival settings/options were restored. World time/weather use the
older partition cache values, and test-generated chunks may remain. This is
not a byte-for-byte restoration of the original disk. The original partition
cache is retained at `build/mc-repro/original-partition-cache.img`. Evidence is under `build/mc-repro/`, including
`minecraft-fixed.png`, `minecraft-fixed-resources.log`, `fixed-latest.log`,
`capacity-boot-fixed.log` and `capacity-baseline-serial.log`.

The host's normal vertex-binding path is enabled by default again. The earlier
NVIDIA legacy-path workaround did not establish a Minecraft fix; it remains an
optional diagnostic via `VIRGL_LEGACY_VERTEX_BINDING=1`. The sparse-attribute,
retired-binding and missing-buffer guard corrections are retained.

## Networking compatibility

Socket interface queries now support SIOCGIFFLAGS, SIOCGIFNETMASK,
SIOCGIFBRDADDR and SIOCGIFHWADDR for the default NIC and loopback. Broadcast
addresses are derived from the configured IPv4 address/netmask; hardware
addresses use the actual device MAC. Loopback reports its own hardware type
and a zero hardware/broadcast address. These fixes address the ENOTTY messages,
independently of GPU allocation.

Earlier fixes retain fault-safe SIOCGIFCONF size queries and TCP SO_LINGER
readback for IPv4/IPv6. The host shader translator explicitly recognizes its
existing fog-semantic handling without changing shader output.

ADDFB2 and KSM messages report normal activity. DRM MODE_CLOSEFB and strict
netlink validation remain unsupported rather than reporting false success.

## Commands

```sh
python3 scripts/tests/test_drm_handle_capacity.py
python3 scripts/tests/test_virgl_boot.py
python3 scripts/tests/test_ifconf.py
python3 scripts/tests/test_socket_compat_boot.py
python3 scripts/tests/test_virgl_host_vertex_validation.py
python3 scripts/tests/test_virgl_host_binding_state.py
```

Start a fresh VM using `make run` to load the rebuilt kernel and local renderer.
