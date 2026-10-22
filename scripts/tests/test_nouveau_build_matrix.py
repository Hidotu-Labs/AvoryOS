#!/usr/bin/env python3
"""Evaluate all four driver gates and actual target-specific Kbuild flags."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
fragment = root / 'kernel/linux/Makefile.kbuild'
samples = {
    'amd': 'drivers/gpu/drm/amd/display/dc/dml/dcn314/dcn314_fpu.c',
    'nouveau': 'drivers/gpu/drm/nouveau/nouveau_drm.c',
    'display': 'drivers/gpu/drm/display/drm_dp_helper.c',
}
with tempfile.TemporaryDirectory(prefix='nouveau-matrix-') as tmp:
    makefile = Path(tmp) / 'Makefile'
    body = f'ARCH := x86_64\ninclude {fragment}\n'
    body += '.PHONY: show\nshow:\n\t@printf "OBJECTS=%s\\n" "$(linux-obj-y)"\n'
    for label, source in samples.items():
        target = 'obj-x86_64/linux/' + source + '.o'
        body += f'.PHONY: {target}\nshow: {target}\n{target}:\n'
        body += f'\t@printf "{label}=%s\\n" "$(LINUX_CPPFLAGS) $(LINUX_CFLAGS)"\n'
    makefile.write_text(body)
    for amd in (0, 1):
        for nouveau in (0, 1):
            result = subprocess.check_output(['make', '-s', '-f', str(makefile), 'show',
                f'KPI_AMDGPU={amd}', f'KPI_NOUVEAU={nouveau}'], cwd=tmp, text=True)
            lines = dict(line.split('=', 1) for line in result.splitlines())
            objects = lines['OBJECTS'].split()
            assert len(objects) == len(set(objects)), 'duplicate linked sources'
            counts = {
                'amd': sum('/drm/amd/' in obj for obj in objects),
                'nouveau': sum('/drm/nouveau/' in obj for obj in objects),
                'display': sum('/drm/display/' in obj for obj in objects),
            }
            assert bool(counts['amd']) == bool(amd)
            assert bool(counts['nouveau']) == bool(nouveau)
            assert bool(counts['display']) == bool(amd or nouveau)
            assert '/nouveau/' not in lines['amd']
            assert '/amd/' not in lines['nouveau']
            assert '/amd/' not in lines['display'] and '/nouveau/' not in lines['display']
            if nouveau:
                assert '/nouveau/include' in lines['nouveau']
                assert '/nouveau/nvkm' in lines['nouveau']
            if amd:
                assert '-msse' in lines['amd'], 'AMD FPU flags lost'
            print(f'PASS: AMD={amd} Nouveau={nouveau}: {counts}; scoped includes/FPU flags')
