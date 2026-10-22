#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#ifndef __user
#define __user
#endif
#include <nouveau_drm.h>

#define INVENTORY_LIMIT 256
#define EDID_LIMIT 32768

static int failed(const char *stage) {
    fprintf(stderr, "NOUVEAU_C6: FAIL %s errno=%d (%s)\n", stage, errno, strerror(errno));
    return 1;
}

static int bounded(uint32_t count) {
    return count <= INVENTORY_LIMIT;
}

static int valid_edid(const unsigned char *data, size_t size) {
    static const unsigned char header[] = {0, 255, 255, 255, 255, 255, 255, 0};
    if (size < 128 || size > EDID_LIMIT || size % 128 ||
        memcmp(data, header, sizeof(header)) || size != ((size_t)data[126] + 1) * 128)
        return 0;
    for (size_t base = 0; base < size; base += 128) {
        unsigned int sum = 0;
        for (size_t offset = 0; offset < 128; offset++)
            sum += data[base + offset];
        if (sum & 255)
            return 0;
    }
    return 1;
}

static int software_test(void) {
    unsigned char data[256] = {0, 255, 255, 255, 255, 255, 255, 0};
    data[127] = 6;
    if (!valid_edid(data, 128) || valid_edid(data, 127) || valid_edid(data, 256) ||
        !bounded(INVENTORY_LIMIT) || bounded(INVENTORY_LIMIT + 1))
        return failed("EDID and bounds oracle");
    data[126] = 1;
    data[127] = 5;
    if (!valid_edid(data, sizeof(data)))
        return failed("EDID extension oracle");
    data[128] ^= 1;
    if (valid_edid(data, sizeof(data)))
        return failed("EDID corruption oracle");
    data[128] ^= 1;
    data[0] = 1;
    if (valid_edid(data, sizeof(data)))
        return failed("EDID header oracle");
    puts("NOUVEAU_C6: SOFTWARE EDID validation and inventory bounds pass; no GPU access");
    return 0;
}

static int find_device(void) {
    int selected = -1;
    for (unsigned int minor = 0; minor < 16; minor++) {
        char path[64], name[64] = {0};
        snprintf(path, sizeof(path), "/dev/dri/card%u", minor);
        int descriptor = open(path, O_RDWR | O_CLOEXEC);
        if (descriptor < 0)
            continue;
        struct drm_version version = {.name_len = sizeof(name) - 1, .name = name};
        struct drm_nouveau_getparam vendor = {.param = NOUVEAU_GETPARAM_PCI_VENDOR};
        struct drm_nouveau_getparam device = {.param = NOUVEAU_GETPARAM_PCI_DEVICE};
        int matches = !ioctl(descriptor, DRM_IOCTL_VERSION, &version) &&
            !strcmp(name, "nouveau") &&
            !ioctl(descriptor, DRM_IOCTL_NOUVEAU_GETPARAM, &vendor) &&
            !ioctl(descriptor, DRM_IOCTL_NOUVEAU_GETPARAM, &device) &&
            vendor.value == 0x10de && device.value == 0x2803;
        if (!matches) {
            if (close(descriptor))
                goto error;
            continue;
        }
        if (selected >= 0) {
            close(descriptor);
            errno = EEXIST;
            goto error;
        }
        selected = descriptor;
        printf("NOUVEAU_C6: device=%s driver=nouveau pci=10de:2803\n", path);
    }
    if (selected < 0)
        errno = ENODEV;
    return selected;
error:
    if (selected >= 0)
        close(selected);
    return -1;
}

static int edid_property(int descriptor, uint32_t property_id, uint64_t value) {
    struct drm_mode_get_property property = {.prop_id = property_id};
    if (ioctl(descriptor, DRM_IOCTL_MODE_GETPROPERTY, &property))
        return failed("GETPROPERTY");
    if (strncmp(property.name, "EDID", sizeof(property.name)))
        return 0;
    if (!(property.flags & DRM_MODE_PROP_BLOB) || value > UINT32_MAX) {
        errno = EPROTO;
        return failed("EDID property type");
    }
    if (!value) {
        puts("NOUVEAU_C6: EDID absent");
        return 0;
    }
    unsigned char data[EDID_LIMIT];
    struct drm_mode_get_blob blob = {.blob_id = value};
    if (ioctl(descriptor, DRM_IOCTL_MODE_GETPROPBLOB, &blob))
        return failed("GETPROPBLOB size");
    uint32_t capacity = blob.length;
    if (!capacity || capacity > sizeof(data)) {
        errno = EOVERFLOW;
        return failed("EDID blob size");
    }
    blob.data = (uintptr_t)data;
    if (ioctl(descriptor, DRM_IOCTL_MODE_GETPROPBLOB, &blob))
        return failed("GETPROPBLOB data");
    if (blob.length != capacity || !valid_edid(data, blob.length)) {
        errno = EPROTO;
        return failed("EDID length/header/checksum");
    }
    printf("NOUVEAU_C6: EDID bytes=%u hex=", blob.length);
    for (uint32_t offset = 0; offset < blob.length; offset++)
        printf("%02x", data[offset]);
    putchar('\n');
    return 0;
}

static int connector_inventory(int descriptor, uint32_t identifier, unsigned int *connected) {
    struct drm_mode_modeinfo modes[INVENTORY_LIMIT];
    uint32_t encoders[INVENTORY_LIMIT], properties[INVENTORY_LIMIT];
    uint64_t values[INVENTORY_LIMIT];
    struct drm_mode_get_connector connector = {.connector_id = identifier};
    if (ioctl(descriptor, DRM_IOCTL_MODE_GETCONNECTOR, &connector))
        return failed("GETCONNECTOR detect");
    connector.count_modes = INVENTORY_LIMIT;
    connector.modes_ptr = (uintptr_t)modes;
    connector.count_encoders = INVENTORY_LIMIT;
    connector.encoders_ptr = (uintptr_t)encoders;
    connector.count_props = INVENTORY_LIMIT;
    connector.props_ptr = (uintptr_t)properties;
    connector.prop_values_ptr = (uintptr_t)values;
    if (ioctl(descriptor, DRM_IOCTL_MODE_GETCONNECTOR, &connector))
        return failed("GETCONNECTOR");
    if (!bounded(connector.count_modes) || !bounded(connector.count_encoders) ||
        !bounded(connector.count_props)) {
        errno = EOVERFLOW;
        return failed("connector inventory capacity; retry with stable topology");
    }
    printf("NOUVEAU_C6: connector=%u type=%u type_id=%u status=%u modes=%u encoder=%u mm=%ux%u\n",
           identifier, connector.connector_type, connector.connector_type_id,
           connector.connection, connector.count_modes, connector.encoder_id,
           connector.mm_width, connector.mm_height);
    if (connector.connection == 1)
        (*connected)++;
    for (uint32_t index = 0; index < connector.count_encoders; index++)
        printf("NOUVEAU_C6: connector=%u possible_encoder=%u\n", identifier, encoders[index]);
    for (uint32_t index = 0; index < connector.count_modes; index++) {
        struct drm_mode_modeinfo *mode = &modes[index];
        printf("NOUVEAU_C6: mode connector=%u name=%.*s size=%ux%u clock_khz=%u refresh=%u flags=%#x type=%#x\n",
               identifier, (int)sizeof(mode->name), mode->name, mode->hdisplay,
               mode->vdisplay, mode->clock, mode->vrefresh, mode->flags, mode->type);
    }
    for (uint32_t index = 0; index < connector.count_props; index++)
        if (edid_property(descriptor, properties[index], values[index]))
            return 1;
    return 0;
}

static int inventory(int descriptor) {
    uint32_t crtcs[INVENTORY_LIMIT], connectors[INVENTORY_LIMIT];
    uint32_t encoders[INVENTORY_LIMIT], framebuffers[INVENTORY_LIMIT];
    struct drm_mode_card_res resources = {
        .crtc_id_ptr = (uintptr_t)crtcs, .count_crtcs = INVENTORY_LIMIT,
        .connector_id_ptr = (uintptr_t)connectors, .count_connectors = INVENTORY_LIMIT,
        .encoder_id_ptr = (uintptr_t)encoders, .count_encoders = INVENTORY_LIMIT,
        .fb_id_ptr = (uintptr_t)framebuffers, .count_fbs = INVENTORY_LIMIT
    };
    if (ioctl(descriptor, DRM_IOCTL_MODE_GETRESOURCES, &resources))
        return failed("GETRESOURCES");
    if (!bounded(resources.count_crtcs) || !bounded(resources.count_connectors) ||
        !bounded(resources.count_encoders) || !bounded(resources.count_fbs)) {
        errno = EOVERFLOW;
        return failed("resource inventory capacity");
    }
    printf("NOUVEAU_C6: resources crtcs=%u connectors=%u encoders=%u fbs=%u max=%ux%u\n",
           resources.count_crtcs, resources.count_connectors, resources.count_encoders,
           resources.count_fbs, resources.max_width, resources.max_height);
    if (!resources.count_crtcs || !resources.count_connectors) {
        errno = ENODEV;
        return failed("missing KMS resources");
    }
    for (uint32_t index = 0; index < resources.count_crtcs; index++) {
        struct drm_mode_crtc crtc = {.crtc_id = crtcs[index]};
        if (ioctl(descriptor, DRM_IOCTL_MODE_GETCRTC, &crtc))
            return failed("GETCRTC");
        printf("NOUVEAU_C6: crtc=%u fb=%u mode_valid=%u\n", crtc.crtc_id, crtc.fb_id, crtc.mode_valid);
    }
    for (uint32_t index = 0; index < resources.count_encoders; index++) {
        struct drm_mode_get_encoder encoder = {.encoder_id = encoders[index]};
        if (ioctl(descriptor, DRM_IOCTL_MODE_GETENCODER, &encoder))
            return failed("GETENCODER");
        printf("NOUVEAU_C6: encoder=%u type=%u crtc=%u possible_crtcs=%#x clones=%#x\n",
               encoder.encoder_id, encoder.encoder_type, encoder.crtc_id,
               encoder.possible_crtcs, encoder.possible_clones);
    }
    unsigned int connected = 0;
    for (uint32_t index = 0; index < resources.count_connectors; index++)
        if (connector_inventory(descriptor, connectors[index], &connected))
            return 1;
    struct drm_set_client_cap capability = {.capability = DRM_CLIENT_CAP_UNIVERSAL_PLANES, .value = 1};
    if (ioctl(descriptor, DRM_IOCTL_SET_CLIENT_CAP, &capability))
        return failed("universal planes capability");
    uint32_t planes[INVENTORY_LIMIT];
    struct drm_mode_get_plane_res plane_resources = {
        .plane_id_ptr = (uintptr_t)planes, .count_planes = INVENTORY_LIMIT
    };
    if (ioctl(descriptor, DRM_IOCTL_MODE_GETPLANERESOURCES, &plane_resources))
        return failed("GETPLANERESOURCES");
    if (!plane_resources.count_planes || !bounded(plane_resources.count_planes)) {
        errno = EOVERFLOW;
        return failed("plane inventory capacity or empty");
    }
    for (uint32_t index = 0; index < plane_resources.count_planes; index++) {
        uint32_t formats[INVENTORY_LIMIT];
        struct drm_mode_get_plane plane = {
            .plane_id = planes[index], .format_type_ptr = (uintptr_t)formats,
            .count_format_types = INVENTORY_LIMIT
        };
        if (ioctl(descriptor, DRM_IOCTL_MODE_GETPLANE, &plane))
            return failed("GETPLANE");
        if (!bounded(plane.count_format_types)) {
            errno = EOVERFLOW;
            return failed("plane format capacity");
        }
        printf("NOUVEAU_C6: plane=%u crtc=%u possible_crtcs=%#x formats=", plane.plane_id,
               plane.crtc_id, plane.possible_crtcs);
        for (uint32_t format = 0; format < plane.count_format_types; format++)
            printf("%08x,", formats[format]);
        putchar('\n');
    }
    printf("NOUVEAU_C6: PASS inventory connected=%u planes=%u; physical pixels not tested\n",
           connected, plane_resources.count_planes);
    return 0;
}

int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "--software"))
        return software_test();
    if (argc != 1)
        return 2;
    setvbuf(stdout, NULL, _IONBF, 0);
    alarm(90);
    puts("NOUVEAU_C6: START inventory");
    int descriptor = find_device();
    if (descriptor < 0)
        return failed("find unique AD106 Nouveau primary node");
    int result = inventory(descriptor);
    if (close(descriptor))
        result = failed("close primary node");
    if (!result)
        puts("NOUVEAU_C6: END inventory status=pass");
    return result;
}
