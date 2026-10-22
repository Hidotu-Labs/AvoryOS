#!/usr/bin/env python3
"""C7 must retain pixel/lifetime evidence and derive Xorg topology from identity."""
import importlib.util
import os
import shutil
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('c7', root/'scripts/nouveau-c7.py')
c7 = importlib.util.module_from_spec(spec)
spec.loader.exec_module(c7)
markers = ['NOUVEAU_C7: START mode=hardware',
           'NOUVEAU_C7: PASS Vulkan device identity and instance teardown',
           'NOUVEAU_C7: END shader pixels and context restart status=pass'] + [
    f'NOUVEAU_C7: PASS pixels cycle={cycle} frame={frame} pixels=4096'
    for cycle in range(3) for frame in range(4)] + [
    f'NOUVEAU_C7: PASS context teardown cycle={cycle}' for cycle in range(3)]
c7.verify('\n'.join(markers))
for failure in ('gsp: rc engn:00000001 chid:4 gfid:0 level:2 type:31',
                'fifo:c00000:0004:0004:[Xorg.wrap[207]] errored - disabling channel',
                'Xorg.wrap[207]: channel 4 killed!',
                'job timeout, channel 4 killed!'):
    for log in (failure + '\n' + '\n'.join(markers),
                '\n'.join(markers) + '\n' + failure):
        try:
            c7.verify(log)
        except RuntimeError:
            pass
        else:
            raise AssertionError(f'GPU failure accepted despite passing pixels: {failure}')
for index in range(len(markers)):
    try:
        c7.verify('\n'.join(markers[:index] + markers[index+1:]))
    except RuntimeError:
        pass
    else:
        raise AssertionError(f'incomplete C7 result accepted: {markers[index]}')
xorg_markers = ['NOUVEAU_C7_XORG: END GLX shader pixels and PRIME presentation status=pass',
                'NOUVEAU_C7_XORG: STRESS updates=256 pixels=pass',
                'NOUVEAU_C7_XORG: VKCUBE frames=600 status=pass',
                'NOUVEAU_C7_XORG: STOP bounded=1 status=0'] + [
    f'NOUVEAU_C7_XORG: READY frame={frame} ' for frame in range(2)] + [
    f'NOUVEAU_C7: PASS pixels cycle=3 frame={frame} pixels=4096' for frame in range(4)]
c7.verify_xorg('\n'.join(markers + xorg_markers))
for index in range(len(xorg_markers)):
    try:
        c7.verify_xorg('\n'.join(markers + xorg_markers[:index] + xorg_markers[index+1:]))
    except RuntimeError:
        pass
    else:
        raise AssertionError(f'incomplete Xorg result accepted: {xorg_markers[index]}')
for log in ('\n'.join(markers) + '\nNOUVEAU_C7: FAIL pixels',
            '\n'.join(markers).replace('mode=hardware', 'mode=software')):
    try:
        c7.verify(log)
    except RuntimeError:
        pass
    else:
        raise AssertionError('failed or software-only hardware qualification accepted')

with tempfile.TemporaryDirectory(prefix='nvk-xorg-') as temp:
    directory = Path(temp)
    fixture = directory/'inventory'
    identify = directory/'identify'
    identify.write_text('#!/bin/sh\ncat "$C7_INVENTORY"\n')
    identify.chmod(0o755)
    def generate(lines):
        fixture.write_text(lines)
        return subprocess.run(['sh', str(root/'config/xorg/avory-nouveau-xorg.sh'),
            str(identify), str(root/'config/xorg/10-nouveau.conf')],
            env=dict(os.environ, C7_INVENTORY=str(fixture)), capture_output=True, text=True)
    native = '/dev/dri/card7 ascentdrm pci:0000:00:0a.0 1 -\n'
    nvidia = '/dev/dri/card3 nouveau pci:0000:02:0b.1 0 /dev/dri/renderD134\n'
    result = generate(nvidia + native)
    assert result.returncode == 0, result.stderr
    for text in ('PCI:2:11:1', 'PCI:0:10:0', '/dev/dri/card7', '/dev/dri/card3', 'MatchDriver "nouveau"'):
        assert text in result.stdout
    assert '@' not in result.stdout
    for invalid in (native, nvidia, native + nvidia + nvidia,
                    native.replace(' 1 -', ' 0 -') + nvidia,
                    native + nvidia.replace('/dev/dri/renderD134', '-'),
                    native + nvidia.replace('0000:02', '0001:02')):
        assert generate(invalid).returncode != 0
    alternate = directory/'alternate'
    alternate.mkdir()
    shutil.copy(root/'config/xorg/10-native.conf', alternate/'10-modesetting-native.conf')
    selector = directory/'nouveau-selector'
    selector.write_text('#!/bin/sh\nprintf "%s\\n" "NVIDIA-generated-profile"\n')
    selector.chmod(0o755)
    icd = directory/'nouveau.json'
    icd.write_text('{}')
    environment = dict(os.environ, AVORY_NOUVEAU_XORG=str(selector), AVORY_NVK_ICD=str(icd))
    environment.pop('AVORY_XORG_PROFILE', None)
    destination = directory/'xorg.conf'
    command = ['sh', str(root/'config/xorg/avory-xorg-profile.sh'),
               str(directory/'missing-sysfs'), str(destination), str(alternate)]
    selected = subprocess.run(command, env=environment, capture_output=True, text=True, check=True)
    assert 'xorg-profile: nouveau' in selected.stdout
    assert destination.read_text().strip() == 'NVIDIA-generated-profile'
    selected = subprocess.run(command, env=dict(environment, AVORY_XORG_PROFILE='native'),
                              capture_output=True, text=True, check=True)
    assert 'xorg-profile: native' in selected.stdout
    environment_script = root/'config/xorg/44-nouveau-env.sh'
    for configuration, expected in ((result.stdout, '1:true'),
                                    ((root/'config/xorg/10-native.conf').read_text(), ':')):
        destination.write_text(configuration)
        policy_environment = dict(os.environ, AVORY_XORG_CONFIG=str(destination))
        policy_environment.pop('NOUVEAU_USE_ZINK', None)
        policy_environment.pop('LIBGL_KOPPER_DRI2', None)
        checked = subprocess.run(['sh', '-c', '. "$1"; printf "%s:%s" "${NOUVEAU_USE_ZINK:-}" "${LIBGL_KOPPER_DRI2:-}"',
                                  'policy', str(environment_script)], env=policy_environment,
                                 capture_output=True, text=True, check=True)
        assert checked.stdout == expected
    destination.write_text(result.stdout)
    checked = subprocess.run(['sh', '-c', '. "$1"; printf "%s" "$LIBGL_KOPPER_DRI2"',
                              'policy', str(environment_script)],
                             env=dict(os.environ, AVORY_XORG_CONFIG=str(destination), LIBGL_KOPPER_DRI2='false'),
                             capture_output=True, text=True, check=True)
    assert checked.stdout == 'false'
    selector.write_text('#!/bin/sh\nexit 1\n')
    selected = subprocess.run(command, env=environment, capture_output=True, text=True, check=True)
    assert 'xorg-profile: native' in selected.stdout
    selected = subprocess.run(command, env=dict(environment, AVORY_XORG_PROFILE='nouveau'),
                              capture_output=True, text=True, check=True)
    assert 'xorg-profile: native' in selected.stdout
print('PASS: strict C7 pixels/restarts and identity-derived NVIDIA/native Xorg topology; ambiguous/unavailable devices rejected')
