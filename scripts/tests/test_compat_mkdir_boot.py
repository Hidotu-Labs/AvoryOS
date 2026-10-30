#!/usr/bin/env python3
"""Check mkdir pathname copying and Linux errno values through int 0x80."""
from test_compat_stat_boot import CHILD, INIT, setup
from boot_probe import run_probe
import test_compat_stat_boot as base

checks = r'''
 CHECK(call(39,(u32)"",0755,0,0,0)==-2);
 CHECK(call(39,1,0755,0,0,0)==-14);
 char large[513];for(unsigned i=0;i<sizeof(large)-1;i++)large[i]='a';
 large[sizeof(large)-1]=0;
 CHECK(call(39,(u32)large,0755,0,0,0)==-36);
 CHECK(call(39,(u32)"/tmp/compat-mkdir",0755,0,0,0)==0);
 CHECK(call(39,(u32)"/tmp/compat-mkdir/",0755,0,0,0)==-17);
'''
base.CHILD = CHILD.replace(' finish(0);', checks + '\n finish(0);')

if __name__ == '__main__':
    run_probe('compat-mkdir-boot', INIT.replace('COMPAT_STAT_BOOT', 'COMPAT_MKDIR_BOOT'),
              'COMPAT_MKDIR_BOOT: PASS', setup=setup, cpus=2, accel='tcg', timeout=90)
