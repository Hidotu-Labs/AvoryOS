#!/usr/bin/env python3
"""C6 inventory qualification must not claim visible physical output."""
from pathlib import Path
import importlib.util
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('nvboot', root/'scripts/nouveau-boot.py')
boot = importlib.util.module_from_spec(spec)
spec.loader.exec_module(boot)
uapi = root/'build/nouveau-618/linux/include/uapi/drm'
with tempfile.TemporaryDirectory(prefix='nouveau-c6-oracle-') as temp:
    binary = Path(temp)/'display'
    subprocess.run(['cc', '-static', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-I', str(uapi), str(root/'userland/test_nouveau_display.c'),
                    '-o', str(binary)], check=True)
    result = subprocess.run([str(binary), '--software'], text=True, capture_output=True, check=True)
    boot.verify_c6_inventory(result.stdout, software=True)
    assert subprocess.run([str(binary), '--unknown'], capture_output=True).returncode == 2
    harness = Path(temp)/'inventory.c'
    harness.write_text(r'''
#include <stdarg.h>
#include <stdlib.h>
#define ioctl inventory_ioctl
#define main display_main
#include "test_nouveau_display.c"
#undef main
#undef ioctl
static int scenario;
static int detects;
int inventory_ioctl(int descriptor, unsigned long command, ...) {
    (void)descriptor;
    va_list arguments;
    va_start(arguments, command);
    void *request = va_arg(arguments, void *);
    va_end(arguments);
    switch (command) {
    case DRM_IOCTL_MODE_GETRESOURCES: {
        struct drm_mode_card_res *resources = request;
        ((uint32_t *)(uintptr_t)resources->crtc_id_ptr)[0] = 1;
        ((uint32_t *)(uintptr_t)resources->connector_id_ptr)[0] = 2;
        ((uint32_t *)(uintptr_t)resources->encoder_id_ptr)[0] = 3;
        resources->count_crtcs = 1;
        resources->count_connectors = scenario == 1 ? INVENTORY_LIMIT + 1 : 1;
        resources->count_encoders = 1;
        resources->count_fbs = 0;
        return 0;
    }
    case DRM_IOCTL_MODE_GETCRTC:
    case DRM_IOCTL_MODE_GETENCODER:
    case DRM_IOCTL_SET_CLIENT_CAP:
        return 0;
    case DRM_IOCTL_MODE_GETCONNECTOR: {
        struct drm_mode_get_connector *connector = request;
        if (!connector->count_modes)
            detects++;
        connector->count_modes = scenario == 2 ? INVENTORY_LIMIT + 1 : 0;
        connector->count_encoders = 0;
        connector->count_props = 1;
        if (connector->props_ptr)
            ((uint32_t *)(uintptr_t)connector->props_ptr)[0] = 4;
        if (connector->prop_values_ptr)
            ((uint64_t *)(uintptr_t)connector->prop_values_ptr)[0] = 5;
        return 0;
    }
    case DRM_IOCTL_MODE_GETPROPERTY: {
        struct drm_mode_get_property *property = request;
        strcpy(property->name, "EDID");
        property->flags = scenario == 3 ? 0 : DRM_MODE_PROP_BLOB;
        return 0;
    }
    case DRM_IOCTL_MODE_GETPROPBLOB: {
        struct drm_mode_get_blob *blob = request;
        blob->length = scenario == 4 ? EDID_LIMIT + 1 : 128;
        if (blob->data) {
            unsigned char *data = (void *)(uintptr_t)blob->data;
            memset(data, 0, 128);
            memset(data + 1, 255, 6);
            data[127] = scenario == 5 ? 7 : 6;
            if (scenario == 6)
                blob->length = 256;
        }
        return 0;
    }
    case DRM_IOCTL_MODE_GETPLANERESOURCES: {
        struct drm_mode_get_plane_res *planes = request;
        ((uint32_t *)(uintptr_t)planes->plane_id_ptr)[0] = 6;
        planes->count_planes = scenario == 7 ? INVENTORY_LIMIT + 1 : 1;
        return 0;
    }
    case DRM_IOCTL_MODE_GETPLANE: {
        struct drm_mode_get_plane *plane = request;
        plane->count_format_types = scenario == 8 ? INVENTORY_LIMIT + 1 : 0;
        if (scenario == 9) {
            errno = EIO;
            return -1;
        }
        return 0;
    }
    default:
        abort();
    }
}
int main(void) {
    if (inventory(42) || detects != 1)
        return 1;
    for (scenario = 1; scenario <= 9; scenario++)
        if (!inventory(42))
            return 2;
    return 0;
}
''')
    mocked = Path(temp)/'inventory'
    subprocess.run(['cc', '-O2', '-Wall', '-Wextra', '-Werror', '-I', str(uapi),
                    '-I', str(root/'userland'), str(harness), '-o', str(mocked)], check=True)
    subprocess.run([str(mocked)], capture_output=True, check=True)

record = dict(kernel_sha256='kernel', cpus=1, firmware={'files':['matched']},
              pcie_root_port=True, gpu_only=True, oracle_kind='c6-inventory',
              oracle_source_sha256='source', oracle_uapi_sha256={'drm_mode.h':'abi'})
ready = dict(record, status='pass', software=True)
assert boot.readiness_matches(ready, record)
for key, value in [('oracle_kind','c5-copy'), ('oracle_source_sha256','old'),
                   ('oracle_uapi_sha256',{'drm_mode.h':'different'})]:
    assert not boot.readiness_matches(dict(ready, **{key:value}), record)
log = ('NOUVEAU_C6: START inventory\n'
       'NOUVEAU_C6: device=/dev/dri/card1 driver=nouveau pci=10de:2803\n'
       'NOUVEAU_C6: resources crtcs=4 connectors=4 encoders=4 fbs=0 max=16384x16384\n'
       'NOUVEAU_C6: connector=1 type=10 status=2 modes=0\n'
       'NOUVEAU_C6: crtc=2 fb=0 mode_valid=0\n'
       'NOUVEAU_C6: plane=3 crtc=0 possible_crtcs=0x1 formats=34325258,\n'
       'NOUVEAU_C6: PASS inventory connected=0 planes=1; physical pixels not tested\n'
       'NOUVEAU_C6: END inventory status=pass\n')
boot.verify_c6_inventory(log)
for marker in ('START inventory', 'driver=nouveau pci=10de:2803', 'resources ',
               'connector=', 'crtc=', 'plane=', 'PASS inventory connected=',
               'END inventory status=pass'):
    try:
        boot.verify_c6_inventory(log.replace(marker, 'missing'))
    except RuntimeError:
        pass
    else:
        raise AssertionError('incomplete inventory accepted: '+marker)
for invalid in (result.stdout, log+'NOUVEAU_C6: FAIL GETCONNECTOR\n'):
    try:
        boot.verify_c6_inventory(invalid)
    except RuntimeError:
        pass
    else:
        raise AssertionError('software-only or failed inventory accepted')
for args in (['--c6-inventory', '--c5-copy'], ['--c6-inventory', '--vfio-only']):
    result = subprocess.run(['python3', str(root/'scripts/nouveau-boot.py'), *args], capture_output=True)
    assert result.returncode == 2
print('PASS: EDID checks, actual inventory fault injection, strict markers, exclusive modes and C6 source/ABI-qualified readiness')
