#!/usr/bin/env python3
"""Exercise the actual desktop selector with identity/topology fixtures."""
import os
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
script = (root/'initrd/drm-pick.sh').read_text()
native = '/dev/dri/card7 ascentdrm pci:0000:00:06.0 1 -\n'
vkms = '/dev/dri/card1 vkms platform:vkms 1 /dev/dri/renderD128\n'
amd = '/dev/dri/card4 amdgpu pci:0000:00:03.0 1 /dev/dri/renderD135\n'
nvidia = '/dev/dri/card2 nouveau pci:0000:01:00.0 0 /dev/dri/renderD129\n'

with tempfile.TemporaryDirectory(prefix='drm-selection-') as temp:
    directory = Path(temp)
    pairing = directory/'pairing.c'
    pairing.write_text('''
#include <assert.h>
#define main identify_main
#include "drm_identify.c"
#undef main
int main(void) {
    struct identity cards[2] = {{.driver="nouveau",.bus="pci:0000:01:00.0"},
                                 {.driver="nouveau",.bus="pci:0000:02:00.0"}};
    struct identity renders[2] = {{.node="/dev/dri/renderD133",.driver="nouveau",.bus="pci:0000:02:00.0"},
                                   {.node="/dev/dri/renderD139",.driver="nouveau",.bus="pci:0000:01:00.0"}};
    pair_nodes(cards,2,renders,2);
    assert(!strcmp(cards[0].render,renders[1].node) && !strcmp(cards[1].render,renders[0].node));
    renders[0].bus[0]=renders[1].bus[0]=0;
    pair_nodes(cards,2,renders,2);assert(!cards[0].render[0] && !cards[1].render[0]);
    pair_nodes(cards,1,renders,1);assert(!strcmp(cards[0].render,renders[0].node));
    strcpy(renders[0].bus,"pci:0000:02:00.0");
    pair_nodes(cards,1,renders,1);assert(!cards[0].render[0]);
    strcpy(renders[0].driver,"amdgpu");
    pair_nodes(cards,1,renders,1);assert(!cards[0].render[0]);
    return 0;
}
''')
    subprocess.run(['cc','-O2','-Wall','-Wextra','-Werror','-I',str(root/'userland'),
                    '-I',str(root/'build/nouveau-618/linux/include/uapi/drm'),str(pairing),'-o',str(directory/'pairing')],check=True)
    subprocess.run([str(directory/'pairing')],check=True)
    helper = directory/'drm-identify'
    records = directory/'inventory'
    cmdline = directory/'cmdline'
    cmdline.write_text('')
    helper.write_text('#!/bin/sh\ncat "'+str(records)+'"\n')
    helper.chmod(0o755)
    selector = directory/'drm-pick.sh'
    selector.write_text(script.replace('/bin/drm-identify',str(helper)).replace('/proc/cmdline',str(cmdline)))
    selector.chmod(0o755)
    def check(inventory, expected, *, settings=None, emulated=False, mode='--pair'):
        records.write_text(inventory)
        cmdline.write_text('kpi_emu_sink=1' if emulated else '')
        environment = {key:value for key,value in os.environ.items() if not key.startswith('ASCENT_DRM_')}
        environment.update(settings or {})
        result = subprocess.run(['sh',str(selector),mode],env=environment,capture_output=True,text=True)
        assert result.stdout.strip() == expected, (inventory,settings,result.stdout,result.stderr)
        assert result.returncode == (0 if expected else 1), (result.returncode,result.stderr)
    check(native+vkms,'/dev/dri/card7 -')
    check(native,'/dev/dri/card7 -')
    check(native+vkms+nvidia,'/dev/dri/card7 /dev/dri/renderD129')
    check(amd+native,'/dev/dri/card4 /dev/dri/renderD135')
    check(nvidia.replace(' 0 ',' 1 '),'/dev/dri/card2 /dev/dri/renderD129')
    check(nvidia,'')
    check(nvidia,'/dev/dri/renderD129',mode='--render')
    check(nvidia+amd+native,'/dev/dri/card4 /dev/dri/renderD135')
    check(native+amd.replace(' 1 ',' 0 ')+nvidia,'/dev/dri/card7 /dev/dri/renderD129')
    check(native+nvidia,'/dev/dri/card7 /dev/dri/renderD129',emulated=True)
    check(native+amd.replace(' 1 ',' 0 ')+nvidia,'/dev/dri/card4 /dev/dri/renderD135',emulated=True)
    for override in ('2','card2','/dev/dri/card2'):
        check(native+nvidia,'/dev/dri/card2 /dev/dri/renderD129',settings={'ASCENT_DRM_CARD':override})
    for bdf in ('0000:01:00.0','pci:0000:01:00.0'):
        check(native+nvidia,'/dev/dri/card2 /dev/dri/renderD129',settings={'ASCENT_DRM_BDF':bdf})
    check(native+nvidia,'/dev/dri/card2 /dev/dri/renderD129',settings={'ASCENT_DRM_DRIVER':'nouveau'})
    for override in ({'ASCENT_DRM_CARD':'card99'}, {'ASCENT_DRM_CARD':'../../tmp/card2'},
                     {'ASCENT_DRM_BDF':'ffff:ff:1f.0'}, {'ASCENT_DRM_DRIVER':'missing'},
                     {'ASCENT_DRM_RENDER':'/dev/dri/renderD999'}):
        check(native+nvidia,'/dev/dri/card7 /dev/dri/renderD129',settings=override)
    check(native+nvidia+amd,'/dev/dri/card4 /dev/dri/renderD129',settings={'ASCENT_DRM_RENDER':'/dev/dri/renderD129'})
    check(vkms,'')
    check('','')
    check(native+vkms+nvidia,
          '/dev/dri/card7 /dev/dri/renderD129 nouveau /dev/dri/card2:/dev/dri/card7',mode='--wayland')
    check(native+vkms+amd,
          '/dev/dri/card4 /dev/dri/renderD135 amdgpu /dev/dri/card4:/dev/dri/card7',mode='--wayland')
    check(native+vkms+amd.replace(' 1 ',' 0 '),
          '/dev/dri/card7 /dev/dri/renderD135 amdgpu /dev/dri/card4:/dev/dri/card7',mode='--wayland')
    check(native+vkms,
          '/dev/dri/card7 - - /dev/dri/card7',mode='--wayland')
    check(native+amd+nvidia,
          '/dev/dri/card4 /dev/dri/renderD129 nouveau /dev/dri/card2:/dev/dri/card4:/dev/dri/card7',
          mode='--wayland',settings={'ASCENT_DRM_RENDER':'/dev/dri/renderD129'})
    check(vkms,'',mode='--wayland')
    check(native+nvidia.replace('/dev/dri/renderD129','-'),'/dev/dri/card7 -')
    check((nvidia+native).replace('card2','card20').replace('card7','card0').replace('renderD129','renderD139'),
          '/dev/dri/card0 /dev/dri/renderD139')
    records.write_text(native+nvidia)
    cmdline.write_text('')
    profile = directory/'avory-drm.sh'
    profile.write_text((root/'initrd/avory-drm.sh').read_text().replace('/bin/drm-pick.sh',str(selector)))
    result = subprocess.run(['sh','-c','. "$1"; printf "%s" "$KWIN_DRM_DEVICES"','sh',str(profile)],
                            env=dict(os.environ,KWIN_DRM_DEVICES='/explicit/card'),capture_output=True,text=True,check=True)
    assert result.stdout == '/explicit/card'
    environment = dict(os.environ)
    environment.pop('KWIN_DRM_DEVICES',None)
    result = subprocess.run(['sh','-c','. "$1"; printf "%s" "$KWIN_DRM_DEVICES"','sh',str(profile)],
                            env=environment,capture_output=True,text=True,check=True)
    assert result.stdout == '/dev/dri/card2:/dev/dri/card7'
    result = subprocess.run(['sh','-c','. "$1"; printf "%s %s" "$KWIN_RENDER_NODES" "$NOUVEAU_USE_ZINK"','sh',str(profile)],
                            env={k:v for k,v in environment.items() if k not in ('KWIN_RENDER_NODES','NOUVEAU_USE_ZINK')},
                            capture_output=True,text=True,check=True)
    assert result.stdout == '/dev/dri/renderD129 1', result
    result = subprocess.run(['sh','-c','. "$1"; printf "%s %s" "$KWIN_RENDER_NODES" "$NOUVEAU_USE_ZINK"','sh',str(profile)],
                            env=dict(environment,KWIN_RENDER_NODES='',NOUVEAU_USE_ZINK='0'),
                            capture_output=True,text=True,check=True)
    assert result.stdout == ' 0', result
    records.write_text(native+vkms+amd.replace(' 1 ',' 0 '))
    clean = {k:v for k,v in environment.items() if k not in ('KWIN_RENDER_NODES','NOUVEAU_USE_ZINK')}
    result = subprocess.run(['sh','-c','. "$1"; printf "%s %s %s" "$KWIN_DRM_DEVICES" "$KWIN_RENDER_NODES" "${NOUVEAU_USE_ZINK-unset}"','sh',str(profile)],
                            env=clean,capture_output=True,text=True,check=True)
    assert result.stdout == '/dev/dri/card4:/dev/dri/card7 /dev/dri/renderD135 unset', result
    records.write_text(native+vkms)
    result = subprocess.run(['sh','-c','. "$1"; printf "%s %s" "$KWIN_DRM_DEVICES" "${KWIN_RENDER_NODES-unset}"','sh',str(profile)],
                            env=clean,capture_output=True,text=True,check=True)
    assert result.stdout == '/dev/dri/card7 unset', result
    records.write_text(native+nvidia)
    xorg = directory/'xorg-profile.sh'
    xorg.write_text((root/'config/xorg/avory-xorg-profile.sh').read_text().replace('/bin/drm-pick.sh',str(selector)))
    alternate = directory/'alternates'
    alternate.mkdir()
    (alternate/'10-modesetting-native.conf').write_text((root/'config/xorg/10-native.conf').read_text())
    destination = directory/'selected.conf'
    subprocess.run(['sh',str(xorg),str(directory/'missing-sys'),str(destination),str(alternate)],
                   env=environment,capture_output=True,check=True)
    assert '"kmsdev" "/dev/dri/card7"' in destination.read_text()
print('PASS: no-NVIDIA/gate-off/native/AMD/NVIDIA/both-driver identity fixtures, reordered minors, overrides, AMD-only emulation, KWin override and Xorg native fallback')
