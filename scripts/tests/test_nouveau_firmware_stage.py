#!/usr/bin/env python3
"""Staging verifies every pinned byte and retains the last good bundle."""
from pathlib import Path
import hashlib,importlib.util,json,tempfile
root=Path(__file__).resolve().parents[2]
spec=importlib.util.spec_from_file_location('stage',root/'scripts/nouveau-firmware-stage.py');m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)
with tempfile.TemporaryDirectory(prefix='nouveau-stage-') as temp:
 base=Path(temp);source=base/'source';(source/'LICENSES').mkdir(parents=True);(source/'LICENSES/LICENCE.nvidia').write_text('test notice')
 (source/'nvidia/canonical').mkdir(parents=True);blob=source/'nvidia/canonical/fw.bin';blob.write_bytes(b'pinned-firmware')
 entry=dict(request='nvidia/ad106/gsp/fw.bin',canonical_source='nvidia/canonical/fw.bin',size=blob.stat().st_size,sha256=hashlib.sha256(blob.read_bytes()).hexdigest())
 lock=base/'lock.json';lock.write_text(json.dumps({'selected':{'firmware':dict(files=[entry],ref='20260916',commit='pinned',gsp_interface='535.113.01',license='LICENSES/LICENCE.nvidia')}}))
 output=base/'output';m.stage(source,output,lock);published=output/'lib/firmware'/entry['request'];before=published.read_bytes();header=(output/'kpi_nouveau_fw_manifest.h').read_bytes()
 assert (output/'lib/firmware/LICENSES/LICENCE.nvidia').is_file()
 for bad in [b'truncated',b'other-firmware!']:
  blob.write_bytes(bad)
  try:m.stage(source,output,lock)
  except RuntimeError:pass
  else:raise AssertionError('accepted bad firmware')
  assert published.read_bytes()==before and (output/'kpi_nouveau_fw_manifest.h').read_bytes()==header
 assert not [p for p in base.glob('.firmware-*') if p.is_dir()]

print("PASS: canonical alias materialization, license, hashes/sizes, bad-bundle rollback and staging cleanup")
