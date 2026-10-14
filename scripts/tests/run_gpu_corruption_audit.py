#!/usr/bin/env python3
"""Run the selected host regressions; this does not exercise AMD hardware."""
from concurrent.futures import ThreadPoolExecutor
import json
from pathlib import Path
import subprocess
import sys

root = Path(__file__).resolve().parents[2]
out = root / 'build/gpu-corruption-audit'
out.mkdir(parents=True, exist_ok=True)
files = sorted((root / 'scripts/tests').glob('test_kpi*.py'))
files += [root / 'scripts/tests' / name for name in (
    'test_linuxkpi_cache_aliases.py', 'test_linuxkpi_file_exports.py',
    'test_native_dma_cache_lifetime.py', 'test_pci_config_transactions.py',
    'test_amdgpu_memory_policy.py', 'test_drm_gem_initialization.py',
    'test_gpu_flight_recorder.py', 'test_gpu_ioctl_trace.py',
    'test_gpu_resource_hooks.py', 'test_gpu_corruption_oracle.py')]


def run(path):
    try:
        result = subprocess.run([sys.executable, str(path)], cwd=root,
                                capture_output=True, text=True, timeout=90)
        return {'test': path.name,
                'status': 'PASS' if result.returncode == 0 else 'FAIL',
                'output': result.stdout + result.stderr}
    except subprocess.TimeoutExpired as error:
        return {'test': path.name, 'status': 'TIMEOUT', 'output': str(error)}


with ThreadPoolExecutor(max_workers=4) as pool:
    results = list(pool.map(run, files))
(out / 'tests.json').write_text(json.dumps(results, indent=2) + '\n')
for result in results:
    print(result['status'], result['test'])
    if result['status'] != 'PASS':
        print(result['output'])
passed = sum(result['status'] == 'PASS' for result in results)
print(f'{passed}/{len(results)} passed')
sys.exit(0 if passed == len(results) else 1)
