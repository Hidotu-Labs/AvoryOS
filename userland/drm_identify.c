#define _GNU_SOURCE
#include <fcntl.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#ifndef __user
#define __user
#endif
#include <drm.h>

struct identity {
    char node[64], driver[64], bus[64], render[64];
    int connected;
};

static int identify(int descriptor, struct identity *entry) {
    struct drm_version version = {.name = entry->driver, .name_len = sizeof(entry->driver) - 1};
    return ioctl(descriptor, DRM_IOCTL_VERSION, &version) || !version.name_len ||
        version.name_len >= sizeof(entry->driver);
}

static int connected(int descriptor) {
    uint32_t connectors[64];
    struct drm_mode_card_res resources = {.connector_id_ptr = (uintptr_t)connectors, .count_connectors = 64};
    if (ioctl(descriptor, DRM_IOCTL_MODE_GETRESOURCES, &resources) || resources.count_connectors > 64)
        return 0;
    for (uint32_t index = 0; index < resources.count_connectors; index++) {
        struct drm_mode_modeinfo mode;
        struct drm_mode_get_connector connector = {.connector_id = connectors[index],
            .count_modes = 1, .modes_ptr = (uintptr_t)&mode};
        if (!ioctl(descriptor, DRM_IOCTL_MODE_GETCONNECTOR, &connector) && connector.connection == 1)
            return 1;
    }
    return 0;
}

static void bus_identity(struct identity *entry) {
    const char *name = strrchr(entry->node, '/');
    char link[128], resolved[4096];
    snprintf(link, sizeof(link), "/sys/class/drm/%s/device", name ? name + 1 : entry->node);
    if (realpath(link, resolved)) {
        const char *base = strrchr(resolved, '/');
        if (base && strlen(base + 1) == 12)
            snprintf(entry->bus, sizeof(entry->bus), "pci:%s", base + 1);
    }
}

static void pair_nodes(struct identity *cards, unsigned int count,
                       const struct identity *renders, unsigned int render_count) {
    for (unsigned int index = 0; index < count; index++) {
        unsigned int matching_cards = 0, matching_renders = 0, bus_matches = 0;
        cards[index].render[0] = 0;
        for (unsigned int peer = 0; peer < count; peer++)
            matching_cards += !strcmp(cards[index].driver, cards[peer].driver);
        for (unsigned int peer = 0; peer < render_count; peer++) {
            if (strcmp(cards[index].driver, renders[peer].driver)) continue;
            matching_renders++;
            if (cards[index].bus[0] && renders[peer].bus[0] && !strcmp(cards[index].bus, renders[peer].bus)) {
                bus_matches++;
                memcpy(cards[index].render, renders[peer].node, sizeof(cards[index].render));
            }
        }
        if (bus_matches != 1) cards[index].render[0] = 0;
        if (!cards[index].render[0] && matching_cards == 1 && matching_renders == 1)
            for (unsigned int peer = 0; peer < render_count; peer++)
                if (!strcmp(cards[index].driver, renders[peer].driver) &&
                    (!renders[peer].bus[0] || !cards[index].bus[0] || !strcmp(cards[index].bus, renders[peer].bus)))
                    memcpy(cards[index].render, renders[peer].node, sizeof(cards[index].render));
    }
}

int main(int argc, char **argv) {
    int verbose = argc == 2 && !strcmp(argv[1], "--verbose");
    if (argc != 1 && !verbose)
        return 2;
    struct identity cards[64] = {0}, renders[64] = {0};
    unsigned int count = 0, render_count = 0;
    for (unsigned int minor = 0; minor < 64; minor++) {
        struct identity entry = {0};
        snprintf(entry.node, sizeof(entry.node), "/dev/dri/card%u", minor);
        int descriptor = open(entry.node, O_RDWR | O_CLOEXEC);
        if (descriptor < 0)
            continue;
        if (!identify(descriptor, &entry)) {
            struct drm_unique unique = {.unique = entry.bus, .unique_len = sizeof(entry.bus) - 1};
            if (ioctl(descriptor, DRM_IOCTL_GET_UNIQUE, &unique) || unique.unique_len >= sizeof(entry.bus))
                entry.bus[0] = 0;
            /* SET_VERSION requires DRM master and changes device state.
             * Identity discovery must also work while a compositor owns it. */
            if (!entry.bus[0])
                bus_identity(&entry);
            entry.connected = connected(descriptor);
            cards[count++] = entry;
        }
        if (close(descriptor)) return 1;
    }
    for (unsigned int minor = 128; minor < 192; minor++) {
        struct identity entry = {0};
        snprintf(entry.node, sizeof(entry.node), "/dev/dri/renderD%u", minor);
        int descriptor = open(entry.node, O_RDWR | O_CLOEXEC);
        if (descriptor < 0) continue;
        if (!identify(descriptor, &entry)) {
            bus_identity(&entry);
            renders[render_count++] = entry;
        }
        if (verbose)
            fprintf(stderr, "drm-identify: node=%s driver=%s bus=%s errno=%d\n", entry.node, entry.driver, entry.bus, errno);
        if (close(descriptor)) return 1;
    }
    pair_nodes(cards, count, renders, render_count);
    for (unsigned int index = 0; index < count; index++) {
        printf("%s %s %s %d %s\n", cards[index].node, cards[index].driver,
               cards[index].bus[0] ? cards[index].bus : "-", cards[index].connected,
               cards[index].render[0] ? cards[index].render : "-");
    }
    return 0;
}
