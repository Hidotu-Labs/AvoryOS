#!/usr/bin/env python3
"""Ensure negative fixtures reach the chosen failure stage and fail closed."""
from pathlib import Path
import importlib.util
import tempfile

ROOT=Path(__file__).resolve().parents[2]
spec=importlib.util.spec_from_file_location('nvboot',ROOT/'scripts/nouveau-boot.py')
module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
names=('gsp','bootloader','booter_load','booter_unload')
with tempfile.TemporaryDirectory(prefix='nouveau-negative-fixtures-') as tmp:
    root=Path(tmp);source=root/'verified';source.mkdir()
    for name in names:
        data=b'g'*(2*1024*1024+17) if name=='gsp' else name.encode()
        (source/f'{name}-570.144.bin').write_bytes(data)
    out=root/'matrix'
    module.stage_failure_bundle(source,out,fixtures=True)
    assert len(list(out.iterdir()))==16
    for absent in names:
        for name in names:
            file=out/f'{name}-c4-{absent}.bin'
            assert file.exists()==(name!=absent)
            if file.exists():
                expected=(source/f'{name}-570.144.bin').read_bytes()
                assert file.read_bytes()==(expected[:2*1024*1024] if name=='gsp' else expected)
        omitted=root/absent
        module.stage_failure_bundle(source,omitted,missing=absent)
        assert {p.name for p in omitted.iterdir()}=={f'{n}-570.144.bin' for n in names if n!=absent}
        good=(f'firmware "nvidia/ad106/gsp/{absent}-570.144.bin" unavailable\n'
              '[NOUVEAU] probe end 0000:00:03.0 result=-2\n'
              '[  OK  ] LinuxKPI: nouveau failed-probe cleanup headers=0/0\n'
              'NOUVEAU_PROBE: expected failed probe exposes no Nouveau DRM card\n')
        module.verify_failed_probe(good,absent)
        for invalid in ('',good.replace('result=-2','result=0'),
                        good.replace('[  OK  ]','[FAIL]'),
                        good+'[drm] Initialized nouveau\n'):
            try:module.verify_failed_probe(invalid,absent)
            except RuntimeError:pass
            else:raise AssertionError('accepted a false negative-test pass')
    assert (source/'gsp-570.144.bin').stat().st_size==2*1024*1024+17
print('PASS: all four negative fixture layouts, vmalloc-sized data, unchanged source, and strict probe-failure evidence')
