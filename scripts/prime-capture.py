#!/usr/bin/env python3
"""Read-only capture of a running Q35 guest's native PRIME source and shadow.

Requires explicit user authorization and root access to /proc/PID/mem.
Only pixel buffers and a small diagnostic JSON are saved; the VM is not paused.
The symbols must match the running guest kernel. This is a diagnostic, not a fix.
"""
from pathlib import Path
import argparse, json, os, struct, subprocess, time

ROOT = Path(__file__).resolve().parents[1]
MASK = 0x000FFFFFFFFFF000
GIB = 1 << 30


def symbols(path):
    result = {}
    for line in subprocess.check_output(['nm', str(path)], text=True).splitlines():
        fields = line.split()
        if len(fields) == 3:
            result[fields[2]] = int(fields[0], 16)
    return result


def kernel_signature(path, address):
    data = path.read_bytes()
    phoff = struct.unpack_from('<Q', data, 32)[0]
    ents, count = struct.unpack_from('<HH', data, 54)
    for i in range(count):
        typ, _, off, va, _, size, _, _ = struct.unpack_from('<IIQQQQQQ', data, phoff+i*ents)
        if typ == 1 and va <= address < va+size:
            return data[off+address-va:off+address-va+64]
    raise RuntimeError('kernel function is outside ELF load segments')


class Guest:
    def __init__(self, pid, kernel):
        cmdline = Path(f'/proc/{pid}/cmdline').read_bytes().split(b'\0')
        if not Path(os.fsdecode(cmdline[0])).name.startswith('qemu-system-'):
            raise RuntimeError('selected process is not QEMU')
        self.fd = os.open(f'/proc/{pid}/mem', os.O_RDONLY)
        maps = []
        for line in Path(f'/proc/{pid}/maps').read_text().splitlines():
            a, b = (int(n, 16) for n in line.split()[0].split('-'))
            if b-a == 4*GIB and line.split()[1].startswith('rw'):
                maps.append(a)
        if len(maps) != 1:
            raise RuntimeError('this diagnostic requires one 4 GiB QEMU RAM allocation')
        self.ram, self.size = maps[0], 4*GIB
        self.sym = symbols(kernel)
        va = self.sym['linuxkpi_prime_begin_cpu']
        signature = kernel_signature(kernel, va)
        found = None
        for offset in range(0, self.size, 16 << 20):
            block = os.pread(self.fd, min((16 << 20)+64, self.size-offset), self.ram+offset)
            at = block.find(signature)
            if at >= 0:
                found = offset+at
                break
        if found is None:
            raise RuntimeError('running kernel does not match the supplied ELF')
        # Limine loads the image contiguously; this is an offset in QEMU RAM.
        self.image = found-(va-0xffffffff80000000)
        self.pml4 = self.raw64(self.image+self.sym['kernel_pml4']-0xffffffff80000000)
        # Infer the Q35 PCI hole from the guest memory map, rather than assuming
        # that guest physical addresses are offsets in the host RAM allocation.
        for below in (2*GIB, 3*GIB, 0xB0000000, 0xC0000000):
            self.below = below
            try:
                response = self.u64(self.sym['memmap_request']+40)
                revision, count, entries = struct.unpack('<QQQ', self.read(response, 24))
                if revision > 2 or not 1 <= count <= 512:
                    continue
                high_end = 0
                for i in range(count):
                    ptr = self.u64(entries+8*i)
                    base, length, kind = struct.unpack('<QQQ', self.read(ptr, 24))
                    if kind in (0, 2, 3, 5, 6) and 4*GIB <= base < 16*GIB:
                        high_end = max(high_end, base+length)
                if high_end and self.size-(high_end-4*GIB) == below:
                    return
            except (OSError, RuntimeError, struct.error):
                continue
        raise RuntimeError('cannot safely identify the guest RAM/PCI-hole layout')

    def raw64(self, offset):
        return struct.unpack('<Q', os.pread(self.fd, 8, self.ram+offset))[0]

    def physical(self, pa, length):
        if pa < self.below:
            offset = pa
            if pa+length > self.below:
                raise RuntimeError('physical read crosses the PCI hole')
        elif pa >= 4*GIB:
            offset = pa-4*GIB+self.below
        else:
            raise RuntimeError('physical address is in the PCI hole')
        if not 0 <= offset <= self.size-length:
            raise RuntimeError('physical read is outside guest RAM')
        result = os.pread(self.fd, length, self.ram+offset)
        if len(result) != length:
            raise RuntimeError('short guest RAM read')
        return result

    def translate(self, va):
        table = self.pml4
        for shift in (39, 30, 21, 12):
            entry = struct.unpack('<Q', self.physical(table+8*((va >> shift) & 511), 8))[0]
            if not entry & 1:
                raise RuntimeError('unmapped guest virtual address')
            if shift in (30, 21) and entry & 128:
                return (entry & MASK & ~((1 << shift)-1))+(va & ((1 << shift)-1))
            table = entry & MASK
        return table+(va & 4095)

    def read(self, va, length):
        chunks = []
        while length:
            n = min(length, 4096-(va & 4095))
            chunks.append(self.physical(self.translate(va), n))
            va += n
            length -= n
        return b''.join(chunks)

    def u64(self, va):
        return struct.unpack('<Q', self.read(va, 8))[0]

    def framebuffer(self):
        # Native ABI offsets, checked against the source headers for this ELF.
        head = self.sym['global_ascentdrm_dev']+48
        node = self.u64(head)
        for _ in range(1024):
            if node == head:
                break
            obj = node-8
            typ = struct.unpack('<I', self.read(obj+4, 4))[0]
            if typ == 0xCCCCCCCC:
                fb = self.u64(obj+312)
                if fb:
                    gem = self.u64(fb+312)
                    if 'scanout_heads' in self.sym:
                        # The replacement engine owns independent scanout
                        # images. ABI: head 48 B, image 24 B, surface 64 B.
                        # Source and selected staging have different strides.
                        state = self.sym['scanout_heads']  # diagnostic supports head 0
                        selected = struct.unpack('<I', self.read(state+24, 4))[0]
                        if self.read(state+28, 1) != b'\0':
                            raise RuntimeError('scanout selection is recovering; retry capture')
                        for surface in (self.u64(state), self.u64(state+8)):
                            if not surface:
                                continue
                            width, height = struct.unpack('<II', self.read(surface, 8))
                            for image in (surface+8, surface+32):
                                if struct.unpack('<I', self.read(image+8, 4))[0] != selected:
                                    continue
                                storage = self.u64(image)
                                if not storage:
                                    continue
                                fw, fh, source_pitch = struct.unpack('<III', self.read(fb+288, 12))
                                if fw != width or fh != height:
                                    raise RuntimeError('capture currently requires an uncropped head-0 framebuffer')
                                offset = struct.unpack('<I', self.read(fb+320, 4))[0]
                                return {'resource':selected, 'width':width, 'height':height,
                                        'pitch':width*4, 'source_pitch':source_pitch,
                                        'source':self.u64(gem+32)+offset,
                                        'shadow':self.u64(storage),
                                        'publication':self.u64(state+32)}
                        raise RuntimeError('no selected immutable scanout image')
                    vg = self.u64(gem+104)
                    if vg:
                        data = self.read(vg, 44)
                        resource, width, height, pitch = struct.unpack_from('<IIII', data)
                        shadow = struct.unpack_from('<Q', data, 32)[0]
                        offset = struct.unpack_from('<I', data, 40)[0]
                        source = self.u64(gem+32)+offset
                        visible = struct.unpack('<I', self.read(fb+288, 4))[0]
                        if shadow and 0 < visible <= width <= 8192 and 0 < height <= 8192 and pitch == width*4:
                            return {'resource':resource, 'width':visible, 'height':height,
                                    'pitch':pitch, 'source':source, 'shadow':shadow}
            node = self.u64(node)
        raise RuntimeError('no active native PRIME scanout framebuffer found')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--pid', type=int)
    parser.add_argument('--kernel', type=Path, default=ROOT/'kernel/bin-x86_64/kernel')
    parser.add_argument('--output', type=Path, default=ROOT/'build/logs/prime-live-capture')
    parser.add_argument('--frames', type=int, default=16)
    parser.add_argument('--delay', type=float, default=0, help='seconds to resume Minecraft before capture starts')
    args = parser.parse_args()
    if not 1 <= args.frames <= 120:
        parser.error('--frames must be between 1 and 120')
    if args.pid is None:
        ids = subprocess.check_output(['pgrep', '-f', '^qemu-system-.*host=0000:0e:00.0'], text=True).split()
        if len(ids) != 1:
            parser.error('use --pid to identify exactly one running GPU VM')
        args.pid = int(ids[0])
    from PIL import Image
    if not 0 <= args.delay <= 60:
        parser.error('--delay must be between 0 and 60 seconds')
    if args.delay:
        print(f'Capture starts in {args.delay:g} seconds; resume the affected program now.', flush=True)
        time.sleep(args.delay)
    guest = Guest(args.pid, args.kernel)
    args.output.mkdir(parents=True, exist_ok=True)
    report = []
    for i in range(args.frames):
        fb = guest.framebuffer()
        length = fb['pitch']*fb['height']
        def read_source():
            if 'source_pitch' not in fb:
                return guest.read(fb['source'], length)
            stride = fb.get('source_pitch', fb['pitch'])
            raw = guest.read(fb['source'], stride*(fb['height']-1)+fb['width']*4)
            return b''.join(raw[y*stride:y*stride+fb['width']*4] for y in range(fb['height'])) if stride != fb['pitch'] else raw
        before = read_source()
        shadow = guest.read(fb['shadow'], length)
        after = read_source()
        stable = before == after and fb == guest.framebuffer()
        pixels_different = sum(before[j:j+3] != shadow[j:j+3]
                               for y in range(fb['height'])
                               for j in range(y*fb['pitch'], y*fb['pitch']+fb['width']*4, 4))
        for name, data in [('source', before), ('shadow', shadow)]:
            im = Image.frombytes('RGBA', (fb['pitch']//4, fb['height']), data, 'raw', 'BGRA')
            im.convert('RGB').crop((0, 0, fb['width'], fb['height'])).save(args.output/f'{i:03d}-{name}.png')
        report.append({'frame':i, 'stable_during_read':stable, 'pixels_different':pixels_different,
                       'width':fb['width'], 'height':fb['height'],
                       'publication':fb.get('publication'), 'resource':fb['resource']})
        print(f'frame {i}: source stable={stable}, source/shadow mismatches={pixels_different}', flush=True)
        time.sleep(0.1)
    (args.output/'report.json').write_text(json.dumps(report, indent=2)+'\n')
    print(f'Captured source and PRIME shadow images in {args.output}')
    print('The producer can be newer than the selected frame; differences can represent successive frames; inspect the captured images before drawing conclusions.')


if __name__ == '__main__':
    try:
        main()
    except (OSError, RuntimeError, subprocess.CalledProcessError) as error:
        raise SystemExit(str(error))
