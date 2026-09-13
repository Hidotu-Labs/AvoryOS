#!/usr/bin/env python3
"""Read/save/replay the passed-through AMD iGPU's frame-buffer registers.

Why this exists
---------------
The Raphael iGPU decodes its UMA frame buffer at the address the platform
firmware programmed at host boot into three GC registers (BAR5 dwords):

    mmGCMC_VM_FB_LOCATION_BASE  0x16fc   (24-bit field, << 24 -> vram_start)
    mmGCMC_VM_FB_LOCATION_TOP   0x16fd
    mmGCMC_VM_FB_OFFSET         0x16e7   (raw << 24 -> vram_base_offset)

which the guest `amdgpu` reads through `gfxhub_v2_1_get_fb_location()` and
`gfxhub_v2_1_get_mc_fb_offset()` (the funcs `gmc_v10_0.c` selects for GC
10.3.6).  A host power event can leave the GC block unprogrammed: every read
returns 0xFFFFFFFF, the guest then computes `VRAM: 512M 0x0000FFFFFF000000`
and trips the imported upstream `BUG_ON` in `gmc_v10_0_get_vm_pde()`
("[BUG] at gmc_v10_0.c:595") during `gfxhub_v2_1_gart_enable()`.

The address exists only in the firmware's register state, so the guest cannot
reconstruct it (there is no VBIOS/ACPI fallback in 6.6).  This tool reads the
registers through VFIO -- no root needed if the user can access the device's
vfio group (`/dev/vfio/<group>`), same as QEMU -- snapshots a good state and
replays it before a run.  QEMU's device reset at VM start was observed not to
clear these registers, so a replay before `qemu-system-*` is effective.

Commands
--------
    scripts/vfio-gpu-fb.py read             # print the registers
    scripts/vfio-gpu-fb.py save             # snapshot a good state
    scripts/vfio-gpu-fb.py restore          # replay the snapshot
    scripts/vfio-gpu-fb.py ensure           # check; snapshot or replay

`make run-c4` runs `ensure` (`VFIO_FB_ENSURE=0` to skip).  A VFIO access
failure is not fatal: it prints a warning and exits 0 so a run is never
blocked by tooling problems.  An unprogrammed device with no snapshot exits
non-zero with instructions -- better than a 20-minute boot into an imported
BUG_ON.
"""

from __future__ import annotations

import argparse
import fcntl
import mmap
import os
import struct
import sys
from datetime import datetime

# ---------------------------------------------------------------------------
# VFIO constants (linux/vfio.h) for x86_64.
# ---------------------------------------------------------------------------
VFIO_TYPE = 0x3B
VFIO_GET_API_VERSION = (VFIO_TYPE << 8) | 0
VFIO_CHECK_EXTENSION = (VFIO_TYPE << 8) | 1
VFIO_SET_IOMMU = (VFIO_TYPE << 8) | 2
VFIO_GROUP_GET_STATUS = (VFIO_TYPE << 8) | 3
VFIO_GROUP_SET_CONTAINER = (VFIO_TYPE << 8) | 4
VFIO_GROUP_GET_DEVICE_FD = (VFIO_TYPE << 8) | 6
VFIO_DEVICE_GET_INFO = (VFIO_TYPE << 8) | 7
VFIO_DEVICE_GET_REGION_INFO = (VFIO_TYPE << 8) | 8

VFIO_API_VERSION = 0
VFIO_GROUP_FLAGS_VIABLE = 1
VFIO_TYPE1_IOMMU = 1
VFIO_PCI_BAR5_REGION_INDEX = 5

# ---------------------------------------------------------------------------
# GC registers (BAR5 dword offsets) -- see gc_10_3_0_offset.h.
# ---------------------------------------------------------------------------
REGS = {
    "FB_OFFSET": 0x16E7,
    "FB_LOCATION_BASE": 0x16FC,
    "FB_LOCATION_TOP": 0x16FD,
    "SYSTEM_APERTURE_LOW": 0x16FE,
    "SYSTEM_APERTURE_HIGH": 0x16FF,
    "AGP_BASE": 0x16F2,
    "AGP_BOT": 0x16F3,
    "AGP_TOP": 0x16F4,
}
SNAPSHOT_REGS = ("FB_LOCATION_BASE", "FB_LOCATION_TOP", "FB_OFFSET")

DEFAULT_BDF = "0000:0e:00.0"
DEFAULT_SNAPSHOT = "build/vfio/fb_regs.txt"


class VfioUnavailable(Exception):
    """VFIO group/device could not be opened (permission, busy, missing)."""


def norm_bdf(bdf: str) -> str:
    """Accept 0e:00.0 and 0000:0e:00.0; return the full sysfs name."""
    parts = bdf.split(":")
    if len(parts) == 2:
        return "0000:" + bdf
    if len(parts) == 3:
        return bdf
    raise ValueError(f"unrecognized BDF {bdf!r}")


class GpuFbRegs:
    """A VFIO group/device + BAR5 mapping for the passed AMD GPU."""

    def __init__(self, bdf: str):
        self.bdf = norm_bdf(bdf)
        self.container = -1
        self.group = -1
        self.dev = -1
        self.bar = None
        self._open()

    # -- VFIO plumbing ------------------------------------------------------
    def _open(self) -> None:
        sysfs = f"/sys/bus/pci/devices/{self.bdf}"
        if not os.path.exists(sysfs):
            raise VfioUnavailable(f"{self.bdf} not present ({sysfs})")
        link = os.path.join(sysfs, "iommu_group")
        if not os.path.exists(link):
            raise VfioUnavailable(f"{self.bdf} has no iommu_group")
        group_id = os.path.basename(os.path.realpath(link))
        group_path = f"/dev/vfio/{group_id}"
        if not os.path.exists(group_path):
            raise VfioUnavailable(f"{group_path} missing (is vfio-pci bound?)")

        try:
            self.container = os.open("/dev/vfio/vfio", os.O_RDWR)
            if fcntl.ioctl(self.container, VFIO_GET_API_VERSION) != VFIO_API_VERSION:
                raise VfioUnavailable("unexpected VFIO API version")
            self.group = os.open(group_path, os.O_RDWR)

            status = bytearray(8)
            fcntl.ioctl(self.group, VFIO_GROUP_GET_STATUS, status)
            _, flags = struct.unpack_from("II", status)
            if not flags & VFIO_GROUP_FLAGS_VIABLE:
                raise VfioUnavailable(f"group {group_id} is not viable")

            fcntl.ioctl(self.group, VFIO_GROUP_SET_CONTAINER,
                        struct.pack("i", self.container))
            fcntl.ioctl(self.container, VFIO_SET_IOMMU,
                        struct.pack("I", VFIO_TYPE1_IOMMU))

            self.dev = fcntl.ioctl(self.group, VFIO_GROUP_GET_DEVICE_FD,
                                   self.bdf.encode())
            if self.dev < 0:
                raise VfioUnavailable("GET_DEVICE_FD failed")

            region = bytearray(32)
            struct.pack_into("IIII", region, 0, 32, 0,
                             VFIO_PCI_BAR5_REGION_INDEX, 0)
            fcntl.ioctl(self.dev, VFIO_DEVICE_GET_REGION_INFO, region)
            _, rflags, index, _, size, offset = struct.unpack_from("IIIIQQ", region)
            if index != VFIO_PCI_BAR5_REGION_INDEX or not size:
                raise VfioUnavailable("BAR5 region unavailable")
            self.bar_size = size
            self.bar = mmap.mmap(self.dev, size,
                                 flags=mmap.MAP_SHARED,
                                 prot=mmap.PROT_READ | mmap.PROT_WRITE,
                                 offset=offset)
        except OSError as exc:
            self.close()
            raise VfioUnavailable(f"VFIO setup for {self.bdf}: {exc}") from exc

    def close(self) -> None:
        if self.bar is not None:
            self.bar.close()
            self.bar = None
        for fd in (self.dev, self.group, self.container):
            if fd >= 0:
                try:
                    os.close(fd)
                except OSError:
                    pass
        self.dev = self.group = self.container = -1

    def __enter__(self) -> "GpuFbRegs":
        return self

    def __exit__(self, *exc) -> None:
        self.close()

    # -- register access ----------------------------------------------------
    def read32(self, off: int) -> int:
        if off * 4 + 4 > self.bar_size:
            raise VfioUnavailable(f"dword 0x{off:x} outside BAR5")
        self.bar.seek(off * 4)
        return struct.unpack("<I", self.bar.read(4))[0]

    def write32(self, off: int, value: int) -> None:
        if off * 4 + 4 > self.bar_size:
            raise VfioUnavailable(f"dword 0x{off:x} outside BAR5")
        self.bar.seek(off * 4)
        self.bar.write(struct.pack("<I", value & 0xFFFFFFFF))

    def read_state(self) -> dict[str, int]:
        return {name: self.read32(off) for name, off in REGS.items()}


def unprogrammed(state: dict[str, int]) -> bool:
    """The GC block reads all-ones when the iGPU was never posted."""
    base = state["FB_LOCATION_BASE"]
    offset = state["FB_OFFSET"]
    return (base == 0xFFFFFFFF or (base & 0x00FFFFFF) == 0x00FFFFFF
            or offset == 0xFFFFFFFF)


def state_line(state: dict[str, int]) -> str:
    return " ".join(f"{name}=0x{state[name]:08x}" for name in SNAPSHOT_REGS)


def save_snapshot(path: str, bdf: str, state: dict[str, int]) -> None:
    os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
    with open(path, "w", encoding="utf-8") as fh:
        fh.write("# AvoryOS VFIO GPU frame-buffer snapshot\n")
        fh.write(f"# bdf {bdf} saved {datetime.now().isoformat(timespec='seconds')}\n")
        for name in SNAPSHOT_REGS:
            fh.write(f"{name}=0x{state[name]:08x}\n")


def load_snapshot(path: str) -> dict[str, int]:
    values: dict[str, int] = {}
    with open(path, "r", encoding="utf-8") as fh:
        for line in fh:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            key, _, val = line.partition("=")
            if key in SNAPSHOT_REGS:
                values[key] = int(val, 16)
    missing = [name for name in SNAPSHOT_REGS if name not in values]
    if missing:
        raise ValueError(f"{path}: missing {', '.join(missing)}")
    return values


def cmd_read(args) -> int:
    with GpuFbRegs(args.bdf) as gpu:
        state = gpu.read_state()
    for name, off in REGS.items():
        print(f"0x{off:04x} {name:20s} = 0x{state[name]:08x}")
    if unprogrammed(state):
        print("\n[vfio-gpu-fb] GC block is UNPROGRAMMED (reads 0xFFFFFFFF).")
        print("[vfio-gpu-fb] The guest amdgpu would BUG at gmc_v10_0.c:595.")
        return 1
    print("\n[vfio-gpu-fb] GPU frame-buffer registers look sane.")
    return 0


def cmd_save(args) -> int:
    with GpuFbRegs(args.bdf) as gpu:
        state = gpu.read_state()
    if unprogrammed(state) and not args.force:
        print(f"[vfio-gpu-fb] refusing to snapshot an unprogrammed GPU "
              f"({state_line(state)})", file=sys.stderr)
        print("[vfio-gpu-fb] reboot/ cold-boot the host so the firmware posts "
              "the iGPU first, then run save again", file=sys.stderr)
        return 1
    save_snapshot(args.file, args.bdf, state)
    print(f"[vfio-gpu-fb] saved {args.file}: {state_line(state)}")
    return 0


def cmd_restore(args) -> int:
    try:
        snapshot = load_snapshot(args.file)
    except OSError as exc:
        print(f"[vfio-gpu-fb] cannot read {args.file}: {exc}", file=sys.stderr)
        return 1
    with GpuFbRegs(args.bdf) as gpu:
        before = gpu.read_state()
        for name in SNAPSHOT_REGS:
            gpu.write32(REGS[name], snapshot[name])
        after = gpu.read_state()
    print(f"[vfio-gpu-fb] before: {state_line(before)}")
    print(f"[vfio-gpu-fb] after:  {state_line(after)}")
    if after["FB_LOCATION_BASE"] != snapshot["FB_LOCATION_BASE"] or \
       after["FB_OFFSET"] != snapshot["FB_OFFSET"]:
        print("[vfio-gpu-fb] replay did not stick -- the GC block is not "
              "powered/posted; a cold host boot with the iGPU enabled is "
              "required (see docs/amdgpu-testing.md)", file=sys.stderr)
        return 1
    print(f"[vfio-gpu-fb] replayed the snapshot from {args.file}")
    return 0


def cmd_ensure(args) -> int:
    try:
        gpu = GpuFbRegs(args.bdf)
    except VfioUnavailable as exc:
        print(f"[vfio-gpu-fb] warning: {exc}", file=sys.stderr)
        print("[vfio-gpu-fb] skipping the frame-buffer check (run anyway)")
        return 0

    with gpu:
        state = gpu.read_state()
        if not unprogrammed(state):
            current = {name: state[name] for name in SNAPSHOT_REGS}
            try:
                old = load_snapshot(args.file)
            except (OSError, ValueError):
                old = None
            if old != current:
                save_snapshot(args.file, args.bdf, current)
                print(f"[vfio-gpu-fb] GPU FB registers OK; snapshot saved to "
                      f"{args.file}")
            else:
                print(f"[vfio-gpu-fb] GPU FB registers OK "
                      f"({state_line(state)})")
            return 0

        print(f"[vfio-gpu-fb] GPU FB registers are unprogrammed: "
              f"{state_line(state)}", file=sys.stderr)
        try:
            snapshot = load_snapshot(args.file)
        except (OSError, ValueError):
            snapshot = None
        if snapshot is None:
            print("[vfio-gpu-fb] no snapshot available.  The guest amdgpu "
                  "would BUG at gmc_v10_0.c:595.", file=sys.stderr)
            print("[vfio-gpu-fb] fix the host state first: cold-boot with the "
                  "iGPU enabled/posted (BIOS 'integrated graphics'/multi-"
                  "monitor on, or a display attached), then:", file=sys.stderr)
            print(f"[vfio-gpu-fb]   python3 scripts/vfio-gpu-fb.py save "
                  f"--bdf {args.bdf}", file=sys.stderr)
            return 1

        for name in SNAPSHOT_REGS:
            gpu.write32(REGS[name], snapshot[name])
        after = gpu.read_state()
        if after["FB_LOCATION_BASE"] != snapshot["FB_LOCATION_BASE"] or \
           after["FB_OFFSET"] != snapshot["FB_OFFSET"]:
            print("[vfio-gpu-fb] replay did not stick -- the GC block is not "
                  "powered; a cold host boot with the iGPU enabled is "
                  "required", file=sys.stderr)
            return 1
        print(f"[vfio-gpu-fb] replayed the FB registers from {args.file}: "
              f"{state_line(after)}")
        return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--bdf", default=os.environ.get("VFIO_BDF", DEFAULT_BDF),
                        help=f"passed GPU (default {DEFAULT_BDF})")
    parser.add_argument("--file", default=DEFAULT_SNAPSHOT,
                        help=f"snapshot file (default {DEFAULT_SNAPSHOT})")
    sub = parser.add_subparsers(dest="command", required=True)

    sub.add_parser("read", help="print the GC frame-buffer registers")
    p_save = sub.add_parser("save", help="snapshot a posted GPU")
    p_save.add_argument("--force", action="store_true",
                        help="save even if the registers read all-ones")
    sub.add_parser("restore", help="replay the snapshot")
    sub.add_parser("ensure", help="snapshot/replay as needed (for run targets)")

    args = parser.parse_args()
    try:
        if args.command == "read":
            return cmd_read(args)
        if args.command == "save":
            return cmd_save(args)
        if args.command == "restore":
            return cmd_restore(args)
        return cmd_ensure(args)
    except VfioUnavailable as exc:
        print(f"[vfio-gpu-fb] warning: {exc}", file=sys.stderr)
        return 0 if args.command == "ensure" else 1
    except OSError as exc:
        print(f"[vfio-gpu-fb] {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
