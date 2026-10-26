#!/usr/bin/env python3
"""Private VM stress for automatic compressed anonymous swap; never opens disk.img."""
import argparse,os
from pathlib import Path
from boot_probe import run_probe
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--cpus',type=int,default=4)
p.add_argument('--memory',default='512M')
p.add_argument('--disabled',action='store_true')
p.add_argument('--limit',action='store_true',help='exhaust a 4 MiB store and verify retained data')
a=p.parse_args()
if a.disabled and a.limit:p.error('choose one variant')
source=(Path(__file__).with_name('zram_boot_stress.c')).read_text()
variant='-off' if a.disabled else '-limit' if a.limit else ''
prefix=('ZRAM_BOOT'+variant.replace('-','_')).upper()+':'
source=('#define EXPECT_DISABLED %d\n#define EXPECT_LIMIT %d\n'%(a.disabled,a.limit))+source.replace('ZRAM_BOOT:',prefix)
run_probe('zram-boot'+variant,source,prefix+' PASS all tests',
 cpus=a.cpus,memory=a.memory,timeout=900,accel=os.environ.get('AVORY_TEST_ACCEL','kvm'),
 kernel_cmdline='kpi_amdgpu=0 init=/bin/probe-init'+(' zram=0' if a.disabled else ' zram_mb=64 zram_limit_mb=4' if a.limit else ''))
