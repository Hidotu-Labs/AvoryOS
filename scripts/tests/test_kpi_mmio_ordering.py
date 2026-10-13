#!/usr/bin/env python3
"""Compile actual native MMIO accessors and check DMA publication ordering.

The compiler may optimize ordinary memory, but cannot hide a descriptor
publication across a device doorbell or retain a pre-completion load.
An optional header argument supports the old-accessor negative control.
"""
from pathlib import Path
import re
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[2]
header = Path(sys.argv[1]) if len(sys.argv) > 1 else root / 'kernel/linuxkpi/include/asm/io.h'
accessors = '/* ── MMIO' + header.read_text().split('/* ── MMIO')[1].split('/* ── port I/O')[0]
source = '#include <stdint.h>\n#include <assert.h>\n#include <stdio.h>\n'
source += 'typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32; typedef uint64_t u64;\n#define __iomem\n'
source += accessors
for width, suffix in [(8,'b'),(16,'w'),(32,'l'),(64,'q')]:
    source += f'''
u{width} payload{width};
void publish{width}(volatile u{width} *restrict doorbell) {{
 payload{width}=17; write{suffix}(7,doorbell); payload{width}=23;
}}
u{width} consume{width}(const volatile u{width} *restrict status) {{
 u{width} old=payload{width}; u{width} completed=read{suffix}(status);
 return payload{width}-old+completed;
}}
'''
source += '''
int main(void){
 u8 b=0;u16 w=0;u32 l=0;u64 q=0;
 writeb(0xa5,&b);writew(0x1234,&w);writel(0x87654321,&l);writeq(0x123456789abcdef0ULL,&q);
 assert(readb(&b)==0xa5 && readw(&w)==0x1234 && readl(&l)==0x87654321 && readq(&q)==0x123456789abcdef0ULL);
 puts("PASS: native MMIO widths and normal-memory publication/completion ordering");
}
'''
with tempfile.TemporaryDirectory(prefix='kpi-mmio-') as tmp:
    src=Path(tmp)/'test.c';asm=Path(tmp)/'test.s';exe=Path(tmp)/'test'
    src.write_text(source)
    subprocess.run(['cc','-O2','-Wall','-Wextra','-Werror','-S',str(src),'-o',str(asm)],check=True)
    text=asm.read_text()
    for width in [8,16,32,64]:
        def body(name):
            return text.split(name+':\n',1)[1].split('.size',1)[0]
        pub=body('publish'+str(width));con=body('consume'+str(width))
        writes=[line for line in pub.splitlines() if re.search(r',\s*payload'+str(width)+r'\(%rip\)',line)]
        reads=[line for line in con.splitlines() if re.search(r'payload'+str(width)+r'\(%rip\),',line)]
        assert len(writes)==2, f'{width}-bit descriptor publication erased across MMIO: {pub}'
        assert len(reads)==2, f'{width}-bit completion reuses stale normal memory: {con}'
        doorbell=next(line for line in pub.splitlines() if '(%rdi)' in line and 'mov' in line)
        status=next(line for line in con.splitlines() if '(%rdi)' in line and 'mov' in line)
        assert pub.index(writes[0]) < pub.index(doorbell) < pub.index(writes[1]), pub
        assert con.index(reads[0]) < con.index(status) < con.index(reads[1]), con
    subprocess.run(['cc','-O2',str(src),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
