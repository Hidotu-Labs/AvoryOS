#include "alsa_emu.h"
#include "audio_dsp.h"
#include "hda.h"
#include "../../console/klog.h"
#include "../../fb/framebuffer.h"
#include "../../fs/ramfs.h"
#include "../../fs/vfs.h"
#include "../../include/arch/uaccess.h"
#include "../../lib/string.h"
#include "../../mm/heap.h"
#include "../../mm/pmm.h"
#include "../../mm/vmm.h"
#include <stdbool.h>
#include <stdint.h>

// ALSA Protocol Versions
#define ALSA_CTL_VERSION 0x00020008
#define ALSA_PCM_VERSION 0x0002000F

// ALSA Control IOCTLs
#define SNDRV_CTL_IOCTL_PVERSION        0x80045500
#define SNDRV_CTL_IOCTL_CARD_INFO       0x81785501
#define SNDRV_CTL_IOCTL_ELEM_LIST       0xC0505510
#define SNDRV_CTL_IOCTL_ELEM_INFO       0xC1105511
#define SNDRV_CTL_IOCTL_ELEM_READ       0xC1085512
#define SNDRV_CTL_IOCTL_ELEM_WRITE      0xC1085513
#define SNDRV_CTL_IOCTL_SUBSCRIBE_EVENTS 0xC0045516
#define SNDRV_CTL_IOCTL_ELEM_ADD        0xC1105517
#define SNDRV_CTL_IOCTL_ELEM_REPLACE    0xC1105518
#define SNDRV_CTL_IOCTL_ELEM_REMOVE     0xC0485519
#define SNDRV_CTL_IOCTL_TLV_READ        0xC008551A
#define SNDRV_CTL_IOCTL_TLV_WRITE       0xC008551B
#define SNDRV_CTL_IOCTL_TLV_COMMAND     0xC008551C
#define SNDRV_CTL_IOCTL_HWDEP_NEXT_DEVICE 0xC0045520
#define SNDRV_CTL_IOCTL_PCM_NEXT_DEVICE 0xC0045530
#define SNDRV_CTL_IOCTL_PCM_INFO        0xC1205531
#define SNDRV_CTL_IOCTL_PCM_PREFER_SUBDEVICE 0x40045532
#define SNDRV_CTL_IOCTL_RAWMIDI_NEXT_DEVICE 0xC0045540

// ALSA PCM IOCTLs
#define SNDRV_PCM_IOCTL_PVERSION        0x80044100
#define SNDRV_PCM_IOCTL_INFO            0x81204101
#define SNDRV_PCM_IOCTL_TSTAMP          0x40044102
#define SNDRV_PCM_IOCTL_TTSTAMP         0x40044103
#define SNDRV_PCM_IOCTL_USER_PVERSION   0x40044104
#define SNDRV_PCM_IOCTL_HW_REFINE       0xC2604110
#define SNDRV_PCM_IOCTL_HW_PARAMS       0xC2604111
#define SNDRV_PCM_IOCTL_HW_FREE         0x00004112
#define SNDRV_PCM_IOCTL_SW_PARAMS       0xC0884113
#define SNDRV_PCM_IOCTL_SW_PARAMS_OLD   0xC0684113
#define SNDRV_PCM_IOCTL_STATUS          0x80984120
#define SNDRV_PCM_IOCTL_DELAY           0x80084121
#define SNDRV_PCM_IOCTL_HWSYNC          0x00004122
#define SNDRV_PCM_IOCTL_SYNC_PTR        0xC0884123
#define SNDRV_PCM_IOCTL_STATUS_EXT      0xC0984124
#define SNDRV_PCM_IOCTL_CHANNEL_INFO    0x80184132
#define SNDRV_PCM_IOCTL_PREPARE         0x00004140
#define SNDRV_PCM_IOCTL_RESET           0x00004141
#define SNDRV_PCM_IOCTL_START           0x00004142
#define SNDRV_PCM_IOCTL_DROP            0x00004143
#define SNDRV_PCM_IOCTL_DRAIN           0x00004144
#define SNDRV_PCM_IOCTL_PAUSE           0x40044145
#define SNDRV_PCM_IOCTL_REWIND          0x40084146
#define SNDRV_PCM_IOCTL_RESUME          0x00004147
#define SNDRV_PCM_IOCTL_XRUN           0x00004148
#define SNDRV_PCM_IOCTL_FORWARD         0x40084149
#define SNDRV_PCM_IOCTL_WRITEI_FRAMES   0x40184150
#define SNDRV_PCM_IOCTL_READI_FRAMES    0x80184151
#define SNDRV_PCM_IOCTL_WRITEN_FRAMES   0x40104152
#define SNDRV_PCM_IOCTL_READN_FRAMES    0x80104153
#define SNDRV_PCM_IOCTL_MMAP_BEGIN      0x80184160
#define SNDRV_PCM_IOCTL_MMAP_COMMIT     0x40184161
#define SNDRV_PCM_IOCTL_LINK            0x40044170
#define SNDRV_PCM_IOCTL_UNLINK          0x00004171

// ALSA PCM mmap special offsets
#define SNDRV_PCM_MMAP_OFFSET_DATA      0x00000000U
#define SNDRV_PCM_MMAP_OFFSET_STATUS    0x80000000U
#define SNDRV_PCM_MMAP_OFFSET_CONTROL   0x81000000U

// ALSA HW_PARAM Indices
#define SNDRV_PCM_HW_PARAM_ACCESS       0
#define SNDRV_PCM_HW_PARAM_FORMAT       1
#define SNDRV_PCM_HW_PARAM_SUBFORMAT    2

#define SNDRV_PCM_HW_PARAM_SAMPLE_BITS  8
#define SNDRV_PCM_HW_PARAM_FRAME_BITS   9
#define SNDRV_PCM_HW_PARAM_CHANNELS     10
#define SNDRV_PCM_HW_PARAM_RATE         11
#define SNDRV_PCM_HW_PARAM_PERIOD_TIME  12
#define SNDRV_PCM_HW_PARAM_PERIOD_SIZE  13
#define SNDRV_PCM_HW_PARAM_PERIOD_BYTES 14
#define SNDRV_PCM_HW_PARAM_PERIODS      15
#define SNDRV_PCM_HW_PARAM_BUFFER_TIME  16
#define SNDRV_PCM_HW_PARAM_BUFFER_SIZE  17
#define SNDRV_PCM_HW_PARAM_BUFFER_BYTES 18
#define SNDRV_PCM_HW_PARAM_TICK_TIME    19

// Data Structures
struct snd_ctl_card_info {
    int card;
    int pad;
    uint8_t id[16];
    uint8_t driver[16];
    uint8_t name[32];
    uint8_t longname[80];
    uint8_t mixername[80];
    uint8_t components[128];
};

struct snd_ctl_elem_id {
    uint32_t numid;
    int32_t iface;
    uint32_t device;
    uint32_t subdevice;
    uint8_t name[44];
    uint32_t index;
};

struct snd_ctl_elem_list {
    uint32_t offset;
    uint32_t space;
    uint32_t used;
    uint32_t count;
    struct snd_ctl_elem_id *pids;
    uint8_t reserved[50];
};

struct snd_ctl_elem_info {
    struct snd_ctl_elem_id id;
    int32_t type;
    uint32_t access;
    uint32_t count;
    int32_t owner;
    union {
        struct {
            long min;
            long max;
            long step;
        } integer;
        struct {
            long long min;
            long long max;
            long long step;
        } integer64;
        struct {
            uint32_t items;
            uint32_t item;
            char name[64];
            uint64_t names_ptr;
            uint32_t names_length;
        } enumerated;
        uint8_t reserved[128];
    } value;
    union {
        uint16_t d[4];
        uint16_t *d_ptr;
    } dimen;
    uint8_t reserved[64 - 4 * sizeof(uint16_t)];
};

struct snd_ctl_elem_value {
    struct snd_ctl_elem_id id;
    uint32_t indirect: 1;
    union {
        union {
            long value[128];
            long *value_ptr;
        } integer;
        union {
            long long value[64];
            long long *value_ptr;
        } integer64;
        union {
            uint32_t item[128];
            uint32_t *item_ptr;
        } enumerated;
        union {
            uint8_t data[512];
            uint8_t *data_ptr;
        } bytes;
        uint8_t reserved_iec[256];
    } value;
    struct {
        uint64_t tv_sec;
        uint64_t tv_nsec;
    } tstamp;
    uint8_t reserved[128 - 16];
};

struct snd_pcm_info {
    uint32_t device;
    uint32_t subdevice;
    int32_t stream;
    int32_t card;
    uint8_t id[64];
    uint8_t name[80];
    uint8_t subname[32];
    int32_t dev_class;
    int32_t dev_subclass;
    uint32_t subdevices_count;
    uint32_t subdevices_avail;
    uint8_t sync[16];
    uint8_t reserved[64];
};

struct snd_interval {
    uint32_t min, max;
    uint32_t openmin:1,
             openmax:1,
             integer:1,
             empty:1;
};

struct snd_mask {
    uint32_t bits[8];
};

struct snd_pcm_hw_params {
    uint32_t flags;
    struct snd_mask masks[3];
    struct snd_mask mres[5];
    struct snd_interval intervals[12];
    struct snd_interval ires[9];
    uint32_t rmask;
    uint32_t cmask;
    uint32_t info;
    uint32_t msbits;
    uint32_t rate_num;
    uint32_t rate_den;
    uint64_t fifo_size;
    uint8_t reserved[64];
};

struct snd_pcm_status {
    int32_t state;
    struct {
        uint64_t tv_sec;
        uint64_t tv_nsec;
    } trigger_tstamp;
    struct {
        uint64_t tv_sec;
        uint64_t tv_nsec;
    } tstamp;
    uint64_t appl_ptr;
    uint64_t hw_ptr;
    int64_t delay;
    uint64_t avail;
    uint64_t avail_max;
    uint64_t overrange;
    int32_t suspended_state;
    uint32_t audio_tstamp_data;
    struct {
        uint64_t tv_sec;
        uint64_t tv_nsec;
    } audio_tstamp;
    struct {
        uint64_t tv_sec;
        uint64_t tv_nsec;
    } driver_tstamp;
    uint32_t audio_tstamp_accuracy;
    uint8_t reserved[52 - 32];
};

struct snd_pcm_mmap_status {
    int32_t state;
    int32_t pad1;
    uint64_t hw_ptr;
    struct {
        uint64_t tv_sec;
        uint64_t tv_nsec;
    } tstamp;
    int32_t suspended_state;
    struct {
        uint64_t tv_sec;
        uint64_t tv_nsec;
    } audio_tstamp;
};

struct snd_pcm_mmap_control {
    uint64_t appl_ptr;
    uint64_t avail_min;
};

struct snd_pcm_sync_ptr {
    uint32_t flags;
    union {
        struct snd_pcm_mmap_status status;
        uint8_t reserved[64];
    } s;
    union {
        struct snd_pcm_mmap_control control;
        uint8_t reserved[64];
    } c;
};

struct snd_pcm_channel_info {
    uint32_t channel;
    uint64_t offset;
    uint32_t first;
    uint32_t step;
};

struct snd_xferi {
    int64_t result;
    void *buf;
    uint64_t frames;
};

struct snd_pcm_mmap_begin_area {
    uint32_t offset;   /* offset in frames */
    uint32_t frames;   /* available frames */
    uint32_t cont;     /* frames until ring wrap */
};

// PCM DMA ring buffer - 512 KiB, allocated once at init
#define ALSA_DMA_BUF_SIZE  (512 * 1024)
#define ALSA_DMA_BUF_PAGES (ALSA_DMA_BUF_SIZE / 4096)
static uint8_t  *alsa_dma_buf_virt = NULL;  // kernel-space virtual address
static uint64_t  alsa_dma_buf_phys = 0;     // physical base
static uint64_t  alsa_mmap_appl_offset = 0; // current write head in frames

// One-page buffers for the ALSA hw plugin's STATUS and CONTROL mmap areas
static uint8_t  *alsa_status_page_virt = NULL;
static uint64_t  alsa_status_page_phys = 0;
static uint8_t  *alsa_control_page_virt = NULL;
static uint64_t  alsa_control_page_phys = 0;

// Internal ALSA state
static uint32_t alsa_sample_rate = 44100;
static uint8_t alsa_channels = 2;
static uint8_t alsa_bits = 16;
static int alsa_pcm_state = 0; // 0 = OPEN, 1 = PREPARED, 2 = RUNNING
static uint64_t alsa_appl_ptr = 0;
static uint64_t alsa_hw_ptr = 0;

// alsa-lib passes 0/1 by value for some _IOW('A', …, int) PCM ioctls (e.g. PAUSE).
static int alsa_ioctl_get_int(uint64_t arg, int *out) {
    if (!out) {
        return -14;
    }
    if (arg <= 1) {
        *out = (int)arg;
        return 0;
    }
    if (!is_user_ptr(arg) || !vmm_is_user_addr_range_valid(arg, sizeof(int))) {
        return -14;
    }
    if (copy_from_user(out, (void *)arg, sizeof(int)) != 0) {
        return -14;
    }
    return 0;
}

// ALSA Control Node Callbacks
static int alsa_ctl_ioctl_internal(struct vfs_node *node, uint32_t request, uint64_t arg) {
    (void)node;
    if (request == SNDRV_CTL_IOCTL_PVERSION) {
        int *ver = (int *)arg;
        if (!ver) return -14;
        *ver = ALSA_CTL_VERSION;
        return 0;
    }

    if (request == SNDRV_CTL_IOCTL_CARD_INFO) {
        struct snd_ctl_card_info *info = (struct snd_ctl_card_info *)arg;
        if (!info) return -14;
        memset(info, 0, sizeof(*info));
        info->card = 0;
        strcpy((char *)info->id, "AvoryOS");
        strcpy((char *)info->driver, "AvoryOS-Audio");
        strcpy((char *)info->name, "AvoryOS Audio");
        strcpy((char *)info->longname, "AvoryOS HD Audio Controller");
        strcpy((char *)info->mixername, "AvoryOS Audio Mixer");
        strcpy((char *)info->components, "HDA:00000000");
        return 0;
    }

    if (request == SNDRV_CTL_IOCTL_PCM_NEXT_DEVICE) {
        int *dev = (int *)arg;
        if (!dev) return -14;
        if (*dev < 0) {
            *dev = 0;
        } else {
            *dev = -1;
        }
        return 0;
    }

    if (request == SNDRV_CTL_IOCTL_HWDEP_NEXT_DEVICE ||
        request == SNDRV_CTL_IOCTL_RAWMIDI_NEXT_DEVICE) {
        int *dev = (int *)arg;
        if (!dev) return -14;
        *dev = -1;
        return 0;
    }

    if (request == SNDRV_CTL_IOCTL_PCM_PREFER_SUBDEVICE ||
        request == SNDRV_CTL_IOCTL_SUBSCRIBE_EVENTS) {
        return 0;
    }

#define MAX_USER_CONTROLS 16

struct user_control {
    bool used;
    struct snd_ctl_elem_id id;
    int type;
    unsigned int access;
    unsigned int count;
    long min;
    long max;
    long step;
    long values[32];
};

static struct user_control user_controls[MAX_USER_CONTROLS];

    if (request == SNDRV_CTL_IOCTL_ELEM_LIST || (request & 0xFFFF) == 0x5510) {
        struct snd_ctl_elem_list *list = (struct snd_ctl_elem_list *)arg;
        if (!list) return -14;
        unsigned int cnt = 0;
        for (int i = 0; i < MAX_USER_CONTROLS; i++) {
            if (user_controls[i].used) cnt++;
        }
        list->count = cnt;
        list->used = 0;
        if (list->pids && list->space > 0) {
            unsigned int idx = 0;
            for (int i = 0; i < MAX_USER_CONTROLS && idx < list->space; i++) {
                if (user_controls[i].used) {
                    memcpy(&list->pids[idx], &user_controls[i].id, sizeof(struct snd_ctl_elem_id));
                    idx++;
                }
            }
            list->used = idx;
        }
        return 0;
    }

    // SNDRV_CTL_IOCTL_ELEM_ADD (0xC1105517), ELEM_REPLACE (0xC1105518)
    if (request == 0xC1105517 || request == 0xC1105518 ||
        (request & 0xFFFF) == 0x5517 || (request & 0xFFFF) == 0x5518) {
        struct snd_ctl_elem_info *info = (struct snd_ctl_elem_info *)arg;
        if (!info) return -14;

        int slot = -1;
        for (int i = 0; i < MAX_USER_CONTROLS; i++) {
            if (user_controls[i].used && strcmp((char *)user_controls[i].id.name, (char *)info->id.name) == 0) {
                slot = i;
                break;
            }
        }
        if (slot == -1) {
            for (int i = 0; i < MAX_USER_CONTROLS; i++) {
                if (!user_controls[i].used) {
                    slot = i;
                    break;
                }
            }
        }
        if (slot == -1) return -12; // -ENOMEM

        user_controls[slot].used = true;
        memcpy(&user_controls[slot].id, &info->id, sizeof(struct snd_ctl_elem_id));
        user_controls[slot].id.numid = slot + 1;
        user_controls[slot].type = info->type ? info->type : 1; // default INTEGER
        user_controls[slot].access = info->access | (1<<0) | (1<<1) | (1<<29); // READWRITE | USER
        user_controls[slot].count = info->count ? info->count : 2;
        user_controls[slot].min = info->value.integer.min;
        user_controls[slot].max = info->value.integer.max ? info->value.integer.max : 255;
        user_controls[slot].step = info->value.integer.step;
        for (int c = 0; c < 32; c++) {
            user_controls[slot].values[c] = user_controls[slot].max; // default full volume
        }

        info->id.numid = slot + 1;
        info->access = user_controls[slot].access;
        return 0;
    }

    if (request == SNDRV_CTL_IOCTL_ELEM_INFO || (request & 0xFFFF) == 0x5511) {
        struct snd_ctl_elem_info *info = (struct snd_ctl_elem_info *)arg;
        if (!info) return -14;

        for (int i = 0; i < MAX_USER_CONTROLS; i++) {
            if (user_controls[i].used &&
                ((info->id.numid && info->id.numid == user_controls[i].id.numid) ||
                 (info->id.name[0] && strcmp((char *)user_controls[i].id.name, (char *)info->id.name) == 0))) {
                memcpy(&info->id, &user_controls[i].id, sizeof(struct snd_ctl_elem_id));
                info->type = user_controls[i].type;
                info->access = user_controls[i].access;
                info->count = user_controls[i].count;
                info->value.integer.min = user_controls[i].min;
                info->value.integer.max = user_controls[i].max;
                info->value.integer.step = user_controls[i].step;
                return 0;
            }
        }
        return -2; // -ENOENT
    }

    if (request == SNDRV_CTL_IOCTL_ELEM_READ || (request & 0xFFFF) == 0x5512) {
        struct snd_ctl_elem_value *val = (struct snd_ctl_elem_value *)arg;
        if (!val) return -14;
        for (int i = 0; i < MAX_USER_CONTROLS; i++) {
            if (user_controls[i].used &&
                ((val->id.numid && val->id.numid == user_controls[i].id.numid) ||
                 (val->id.name[0] && strcmp((char *)user_controls[i].id.name, (char *)val->id.name) == 0))) {
                for (unsigned int c = 0; c < user_controls[i].count && c < 32; c++) {
                    val->value.integer.value[c] = user_controls[i].values[c];
                }
                return 0;
            }
        }
        return -2; // -ENOENT
    }

    if (request == SNDRV_CTL_IOCTL_ELEM_WRITE || (request & 0xFFFF) == 0x5513) {
        struct snd_ctl_elem_value *val = (struct snd_ctl_elem_value *)arg;
        if (!val) return -14;
        for (int i = 0; i < MAX_USER_CONTROLS; i++) {
            if (user_controls[i].used &&
                ((val->id.numid && val->id.numid == user_controls[i].id.numid) ||
                 (val->id.name[0] && strcmp((char *)user_controls[i].id.name, (char *)val->id.name) == 0))) {
                for (unsigned int c = 0; c < user_controls[i].count && c < 32; c++) {
                    user_controls[i].values[c] = val->value.integer.value[c];
                }
                return 0;
            }
        }
        return -2; // -ENOENT
    }

    if ((request & 0xFFFF) == 0x5519) {
        // SNDRV_CTL_IOCTL_ELEM_REMOVE
        struct snd_ctl_elem_id *id = (struct snd_ctl_elem_id *)arg;
        if (id) {
            for (int i = 0; i < MAX_USER_CONTROLS; i++) {
                if (user_controls[i].used &&
                    ((id->numid && id->numid == user_controls[i].id.numid) ||
                     (id->name[0] && strcmp((char *)user_controls[i].id.name, (char *)id->name) == 0))) {
                    user_controls[i].used = false;
                    return 0;
                }
            }
        }
        return 0;
    }

    if (request == SNDRV_CTL_IOCTL_PCM_INFO || (request & 0xFFFF) == 0x5531) {
        struct snd_pcm_info *info = (struct snd_pcm_info *)arg;
        if (!info) return -14;
        memset(info, 0, sizeof(*info));
        info->device = 0;
        info->subdevice = 0;
        info->stream = 0;
        info->card = 0;
        strcpy((char *)info->id, "HDA PCM");
        strcpy((char *)info->name, "HDA Intel PCM");
        strcpy((char *)info->subname, "subdevice #0");
        info->subdevices_count = 1;
        info->subdevices_avail = 1;
        return 0;
    }

    if (request == SNDRV_CTL_IOCTL_TLV_READ || request == SNDRV_CTL_IOCTL_TLV_WRITE ||
        (request & 0xFF00) == 0x5500) {
        return 0; // Silently handle any other control ioctls
    }

    return -25; // -ENOTTY (Inappropriate ioctl for device)
}

static int alsa_ctl_ioctl(struct vfs_node *node, uint32_t request, uint64_t arg) {
    return alsa_ctl_ioctl_internal(node, request, arg);
}

static void alsa_set_interval(struct snd_interval *it, uint32_t val) {
    it->min = val;
    it->max = val;
    it->openmin = 0;
    it->openmax = 0;
    it->integer = 1;
    it->empty = 0;
}

static uint8_t alsa_format_to_bits(uint32_t fmt_mask) {
    if (!fmt_mask) {
        return 16;
    }
    uint32_t fmt = 1U << __builtin_ctz(fmt_mask);
    if (fmt & ((1U << 10) | (1U << 14))) {
        return 32;
    }
    if (fmt & (1U << 6)) {
        return 24;
    }
    if (fmt & ((1U << 0) | (1U << 1))) {
        return 8;
    }
    return 16;
}

static bool alsa_interval_exact(const struct snd_interval *it) {
    return it && !it->empty && it->min == it->max;
}

// Keep period/buffer/periods and derived byte/time fields consistent.
// alsa-lib rejects HW_PARAMS when buffer_size is not an exact multiple of
// period_size even if both intervals were individually refined to min==max.
static void alsa_reconcile_hw_params(struct snd_pcm_hw_params *params, bool commit) {
    struct snd_interval *sample_bits = &params->intervals[0];
    struct snd_interval *frame_bits  = &params->intervals[1];
    struct snd_interval *channels    = &params->intervals[2];
    struct snd_interval *rate        = &params->intervals[3];
    struct snd_interval *period_time = &params->intervals[4];
    struct snd_interval *period_size = &params->intervals[5];
    struct snd_interval *period_bytes= &params->intervals[6];
    struct snd_interval *periods_it  = &params->intervals[7];
    struct snd_interval *buffer_time = &params->intervals[8];
    struct snd_interval *buffer_size = &params->intervals[9];
    struct snd_interval *buffer_bytes= &params->intervals[10];

    uint8_t bits = alsa_format_to_bits(params->masks[1].bits[0]);

    uint32_t ch = 2;
    if (!channels->empty) {
        if (channels->min == channels->max) {
            ch = channels->min;
        } else if (channels->min >= 1 && channels->min <= 2) {
            ch = channels->min;
        } else if (channels->max >= 1 && channels->max <= 2) {
            ch = channels->max;
        }
    }

    uint32_t r = 44100;
    if (!rate->empty) {
        if (rate->min == rate->max) {
            r = rate->min;
        } else if (rate->min >= 8000 && rate->min <= 192000) {
            r = rate->min;
        } else if (rate->max >= 8000 && rate->max <= 192000) {
            r = rate->max;
        }
    }

    uint32_t frame_bytes = ((uint32_t)bits / 8U) * ch;
    if (frame_bytes == 0) {
        frame_bytes = 4;
    }
    uint32_t max_buf_frames = ALSA_DMA_BUF_SIZE / frame_bytes;

    uint32_t psize = 0;
    if (!period_size->empty) {
        if (period_size->min == period_size->max && period_size->min >= 16) {
            psize = period_size->min;
        } else if (period_size->min >= 16 && period_size->min <= 65536) {
            psize = period_size->min;
        } else if (period_size->max >= 16 && period_size->max <= 65536) {
            psize = period_size->max;
        }
    }
    if (psize == 0 && !period_time->empty && period_time->min != 0 && r != 0) {
        psize = (uint32_t)((uint64_t)period_time->min * r / 1000000ULL);
    }
    if (psize < 64) {
        psize = 1024;
    }
    if (psize > 16384) {
        psize = 16384;
    }
    if (psize > max_buf_frames / 2) {
        psize = max_buf_frames / 2;
    }

    uint32_t bsize = 0;
    uint32_t nperiods = 4;
    if (!periods_it->empty && periods_it->min == periods_it->max && periods_it->min >= 2) {
        nperiods = periods_it->min;
    }

    if (!buffer_size->empty) {
        if (buffer_size->min == buffer_size->max && buffer_size->min >= psize) {
            bsize = buffer_size->min;
        } else if (buffer_size->min > psize && buffer_size->min <= max_buf_frames) {
            bsize = buffer_size->min;
        } else if (buffer_size->max >= psize * 2 && buffer_size->max <= max_buf_frames) {
            bsize = buffer_size->max;
        }
    }
    if (bsize == 0 && !buffer_time->empty && buffer_time->min != 0 && r != 0) {
        bsize = (uint32_t)((uint64_t)buffer_time->min * r / 1000000ULL);
    }
    if (bsize == 0) {
        bsize = psize * nperiods;
    }

    nperiods = bsize / psize;
    if (nperiods < 2) {
        nperiods = 2;
    }
    bsize = psize * nperiods;
    if (bsize > max_buf_frames) {
        nperiods = max_buf_frames / psize;
        if (nperiods < 2) {
            psize = max_buf_frames / 4;
            if (psize < 64) {
                psize = 64;
            }
            nperiods = max_buf_frames / psize;
            if (nperiods < 2) {
                nperiods = 2;
            }
        }
        bsize = psize * nperiods;
    }

    uint32_t ptime = (r != 0) ? (uint32_t)((uint64_t)psize * 1000000ULL / r) : 0;
    uint32_t pbytes = psize * frame_bytes;
    uint32_t btime = (r != 0) ? (uint32_t)((uint64_t)bsize * 1000000ULL / r) : 0;
    uint32_t bbytes = bsize * frame_bytes;
    uint32_t fb = frame_bytes * 8;

    bool period_exact = alsa_interval_exact(period_size);
    bool buffer_exact = alsa_interval_exact(buffer_size);
    bool periods_exact = alsa_interval_exact(periods_it);
    bool ring_locked = period_exact || buffer_exact || periods_exact;

    if (!commit && !ring_locked) {
        return;
    }

    alsa_set_interval(sample_bits, bits);
    alsa_set_interval(frame_bits, fb);
    if (!channels->empty) {
        alsa_set_interval(channels, ch);
    }
    if (!rate->empty) {
        alsa_set_interval(rate, r);
    }
    alsa_set_interval(period_size, psize);
    alsa_set_interval(period_time, ptime);
    alsa_set_interval(period_bytes, pbytes);
    alsa_set_interval(periods_it, nperiods);
    alsa_set_interval(buffer_size, bsize);
    alsa_set_interval(buffer_time, btime);
    alsa_set_interval(buffer_bytes, bbytes);
    alsa_set_interval(&params->intervals[11], 0);
}

// ALSA PCM Node Callbacks
static int alsa_pcm_ioctl_internal(struct vfs_node *node, uint32_t request, uint64_t arg) {
    (void)node;

    if (request == SNDRV_PCM_IOCTL_PVERSION) {
        int *ver = (int *)arg;
        if (!ver) return -14;
        *ver = ALSA_PCM_VERSION;
        return 0;
    }

    if (request == SNDRV_PCM_IOCTL_INFO) {
        struct snd_pcm_info *info = (struct snd_pcm_info *)arg;
        if (!info) return -14;
        memset(info, 0, sizeof(*info));
        info->device = 0;
        info->subdevice = 0;
        info->stream = 0; // Playback
        info->card = 0;
        strcpy((char *)info->id, "HDA PCM");
        strcpy((char *)info->name, "HDA Intel PCM");
        strcpy((char *)info->subname, "subdevice #0");
        info->subdevices_count = 1;
        info->subdevices_avail = 1;
        return 0;
    }

    if (request == SNDRV_PCM_IOCTL_TSTAMP || request == SNDRV_PCM_IOCTL_TTSTAMP ||
        request == SNDRV_PCM_IOCTL_USER_PVERSION) {
        // USER_PVERSION: client informs us of its ALSA protocol version — acknowledge silently
        return 0;
    }

    if (request == SNDRV_PCM_IOCTL_HW_REFINE) {
        struct snd_pcm_hw_params *params = (struct snd_pcm_hw_params *)arg;
        if (!params) return -14;

        // ALSA access enum: MMAP_INTERLEAVED=0, MMAP_NONINTERLEAVED=1, MMAP_COMPLEX=2,
        //                   RW_INTERLEAVED=3, RW_NONINTERLEAVED=4
        uint32_t supported_access = (1U << 0) | (1U << 1) | (1U << 2) | (1U << 3) | (1U << 4);
        uint32_t supported_format = (1U << 0) | (1U << 1) | (1U << 2) | (1U << 3) | (1U << 6) | (1U << 10) | (1U << 14);
        uint32_t supported_subformat = (1U << 0);

        if (params->masks[0].bits[0]) {
            params->masks[0].bits[0] &= supported_access;
            if (!params->masks[0].bits[0]) params->masks[0].bits[0] = supported_access;
        } else {
            params->masks[0].bits[0] = supported_access;
        }
        for (int b = 1; b < 8; b++) params->masks[0].bits[b] = 0;

        if (params->masks[1].bits[0]) {
            params->masks[1].bits[0] &= supported_format;
            if (!params->masks[1].bits[0]) params->masks[1].bits[0] = (1U << 2);
        } else {
            params->masks[1].bits[0] = supported_format;
        }
        for (int b = 1; b < 8; b++) params->masks[1].bits[b] = 0;

        if (params->masks[2].bits[0]) {
            params->masks[2].bits[0] &= supported_subformat;
            if (!params->masks[2].bits[0]) params->masks[2].bits[0] = (1U << 0);
        } else {
            params->masks[2].bits[0] = supported_subformat;
        }
        for (int b = 1; b < 8; b++) params->masks[2].bits[b] = 0;

        #define REFINE_INTERVAL(idx, hw_min, hw_max) do { \
            struct snd_interval *it = &params->intervals[idx]; \
            uint32_t _hmin = (uint32_t)(hw_min); \
            uint32_t _hmax = (uint32_t)(hw_max); \
            if (it->empty) { \
                it->min = _hmin; it->max = _hmax; it->empty = 0; \
            } else { \
                if (it->min < _hmin) it->min = _hmin; \
                if (it->max == 0 || it->max > _hmax) it->max = _hmax; \
                if (it->min > it->max) it->min = it->max; \
            } \
            it->openmin = 0; it->openmax = 0; it->integer = 1; \
        } while (0)

        REFINE_INTERVAL(0, 8, 32);          // SAMPLE_BITS
        REFINE_INTERVAL(1, 8, 64);          // FRAME_BITS
        REFINE_INTERVAL(2, 1, 2);           // CHANNELS
        REFINE_INTERVAL(3, 8000, 192000);   // RATE
        REFINE_INTERVAL(4, 100, 10000000);  // PERIOD_TIME
        REFINE_INTERVAL(5, 16, 65536);      // PERIOD_SIZE
        REFINE_INTERVAL(6, 64, 262144);     // PERIOD_BYTES
        REFINE_INTERVAL(7, 2, 1024);        // PERIODS
        REFINE_INTERVAL(8, 1000, 10000000); // BUFFER_TIME
        REFINE_INTERVAL(9, 32, 131072);     // BUFFER_SIZE
        REFINE_INTERVAL(10, 128, 524288);   // BUFFER_BYTES
        REFINE_INTERVAL(11, 0, 0);          // TICK_TIME
        #undef REFINE_INTERVAL

        params->info = 0x00000001 | 0x00000002 | 0x00000100 | 0x00000200 |
                       0x00010000 | 0x00040000 | 0x00080000;
        params->msbits = 16;
        params->rate_num = alsa_sample_rate ? alsa_sample_rate : 44100;
        params->rate_den = 1;
        params->fifo_size = 0;
        params->cmask = params->rmask ? params->rmask : 0xFFFFFFFF;
        params->rmask = 0;

        alsa_reconcile_hw_params(params, false);

        return 0;
    }

    if (request == SNDRV_PCM_IOCTL_HW_PARAMS) {
        struct snd_pcm_hw_params *params = (struct snd_pcm_hw_params *)arg;
        if (!params) return -14;

        // 1. Collapse ACCESS to a single bit
        uint32_t chosen_access = (1U << 3); // RW_INTERLEAVED
        if (params->masks[0].bits[0]) {
            chosen_access = 1U << (__builtin_ctz(params->masks[0].bits[0]));
        }
        params->masks[0].bits[0] = chosen_access;
        for (int b = 1; b < 8; b++) params->masks[0].bits[b] = 0;

        // 2. Collapse FORMAT to a single bit
        uint32_t chosen_format = (1U << 2); // S16_LE
        if (params->masks[1].bits[0]) {
            chosen_format = 1U << (__builtin_ctz(params->masks[1].bits[0]));
        }
        params->masks[1].bits[0] = chosen_format;
        for (int b = 1; b < 8; b++) params->masks[1].bits[b] = 0;

        // 3. Collapse SUBFORMAT
        params->masks[2].bits[0] = (1U << 0);
        for (int b = 1; b < 8; b++) params->masks[2].bits[b] = 0;

        alsa_reconcile_hw_params(params, true);

        alsa_bits = alsa_format_to_bits(params->masks[1].bits[0]);
        alsa_channels = (uint8_t)params->intervals[2].min;
        alsa_sample_rate = params->intervals[3].min;

        params->rmask = 0;
        params->cmask = 0;
        params->info = 0x00000001 | 0x00000002 | 0x00000100 | 0x00000200 |
                       0x00010000 | 0x00040000 | 0x00080000;
        params->flags &= ~0x4U;
        params->msbits = alsa_bits;
        params->rate_num = alsa_sample_rate;
        params->rate_den = 1;
        params->fifo_size = 0;

        if (hda_is_present()) {
            hda_set_format(alsa_sample_rate, alsa_channels, alsa_bits);
            hda_reset_stream();
        }
        alsa_pcm_state = 1; // SNDRV_PCM_STATE_SETUP
        alsa_appl_ptr = 0;
        alsa_hw_ptr = 0;
        alsa_mmap_appl_offset = 0;
        if (alsa_status_page_virt) {
            ((struct snd_pcm_mmap_status *)alsa_status_page_virt)->state = alsa_pcm_state;
            ((struct snd_pcm_mmap_status *)alsa_status_page_virt)->hw_ptr = 0;
        }
        if (alsa_control_page_virt) {
            ((struct snd_pcm_mmap_control *)alsa_control_page_virt)->appl_ptr = 0;
        }
        return 0;
    }

    if (request == SNDRV_PCM_IOCTL_HW_FREE) {
        return 0;
    }

    if (request == SNDRV_PCM_IOCTL_SW_PARAMS || request == SNDRV_PCM_IOCTL_SW_PARAMS_OLD ||
        (request & 0xFFFF) == 0x4113) {
        return 0;
    }

    if (request == SNDRV_PCM_IOCTL_CHANNEL_INFO) {
        struct snd_pcm_channel_info *ch = (struct snd_pcm_channel_info *)arg;
        if (!ch) return -14;
        ch->offset = 0; // Page-aligned byte offset for mmap (0 for interleaved DMA buffer)
        ch->first = ch->channel * alsa_bits; // Bit offset of this channel within the frame (0 or 16)
        ch->step = alsa_channels * alsa_bits; // Bit step between samples for this channel (32 bits)
        return 0;
    }

    if (request == SNDRV_PCM_IOCTL_DELAY) {
        int64_t *delay = (int64_t *)arg;
        if (!delay) return -14;
        uint32_t frame_size = (alsa_bits / 8) * alsa_channels;
        if (frame_size == 0) frame_size = 4;
        uint32_t queued = hda_is_present() ? hda_get_ring_count() : 0;
        uint64_t queued_frames = queued / frame_size;
        if (queued_frames > alsa_appl_ptr) queued_frames = alsa_appl_ptr;
        *delay = (int64_t)queued_frames;
        return 0;
    }

    if (request == SNDRV_PCM_IOCTL_SYNC_PTR || (request & 0xFFFF) == 0x4123) {
        struct snd_pcm_sync_ptr *sync = (struct snd_pcm_sync_ptr *)arg;
        if (!sync) return -14;

        if (!(sync->flags & (1U << 1) /* SNDRV_PCM_SYNC_PTR_APPL */)) {
            alsa_appl_ptr = sync->c.control.appl_ptr;
        } else {
            sync->c.control.appl_ptr = alsa_appl_ptr;
        }
        if (!(sync->flags & (1U << 2) /* SNDRV_PCM_SYNC_PTR_AVAIL_MIN */)) {
            // Keep user avail_min
        } else {
            sync->c.control.avail_min = 1;
        }

        sync->s.status.state = alsa_pcm_state;
        uint32_t frame_size = (alsa_bits / 8) * alsa_channels;
        if (frame_size == 0) frame_size = 4;
        uint32_t queued_bytes = hda_is_present() ? hda_get_ring_count() : 0;
        uint64_t queued_frames = queued_bytes / frame_size;
        if (queued_frames > alsa_appl_ptr) queued_frames = alsa_appl_ptr;
        sync->s.status.hw_ptr = alsa_appl_ptr - queued_frames;
        return 0;
    }

    if (request == SNDRV_PCM_IOCTL_PREPARE) {
        if (hda_is_present()) {
            hda_reset_stream();
        }
        alsa_pcm_state = 2; // SNDRV_PCM_STATE_PREPARED
        alsa_appl_ptr = 0;
        alsa_hw_ptr = 0;
        if (alsa_status_page_virt) {
            ((struct snd_pcm_mmap_status *)alsa_status_page_virt)->state = alsa_pcm_state;
            ((struct snd_pcm_mmap_status *)alsa_status_page_virt)->hw_ptr = 0;
        }
        if (alsa_control_page_virt) {
            ((struct snd_pcm_mmap_control *)alsa_control_page_virt)->appl_ptr = 0;
        }
        return 0;
    }

    if (request == SNDRV_PCM_IOCTL_START) {
        alsa_pcm_state = 3; // SNDRV_PCM_STATE_RUNNING
        if (alsa_status_page_virt) {
            ((struct snd_pcm_mmap_status *)alsa_status_page_virt)->state = alsa_pcm_state;
        }
        return 0;
    }

    if (request == SNDRV_PCM_IOCTL_DROP || request == SNDRV_PCM_IOCTL_RESET) {
        if (hda_is_present()) {
            hda_reset_stream();
        }
        alsa_pcm_state = 1; // SNDRV_PCM_STATE_SETUP
        alsa_appl_ptr = 0;
        alsa_hw_ptr = 0;
        if (alsa_status_page_virt) {
            ((struct snd_pcm_mmap_status *)alsa_status_page_virt)->state = alsa_pcm_state;
            ((struct snd_pcm_mmap_status *)alsa_status_page_virt)->hw_ptr = 0;
        }
        if (alsa_control_page_virt) {
            ((struct snd_pcm_mmap_control *)alsa_control_page_virt)->appl_ptr = 0;
        }
        return 0;
    }

    if (request == SNDRV_PCM_IOCTL_DRAIN || request == SNDRV_PCM_IOCTL_HWSYNC) {
        return 0;
    }

    if (request == SNDRV_PCM_IOCTL_PAUSE) {
        int enable = 0;
        int ret = alsa_ioctl_get_int(arg, &enable);
        if (ret < 0) {
            return ret;
        }
        if (enable) {
            alsa_pcm_state = 5; // SNDRV_PCM_STATE_PAUSED
        } else {
            alsa_pcm_state = 3; // SNDRV_PCM_STATE_RUNNING
        }
        if (alsa_status_page_virt) {
            ((struct snd_pcm_mmap_status *)alsa_status_page_virt)->state = alsa_pcm_state;
        }
        return 0;
    }

    if (request == SNDRV_PCM_IOCTL_WRITEI_FRAMES) {
        struct snd_xferi *xferi = (struct snd_xferi *)arg;
        if (!xferi || !xferi->buf) return -14;

        uint32_t frame_size = (alsa_bits / 8) * alsa_channels;
        uint32_t total_bytes = (uint32_t)(xferi->frames * frame_size);

        uint32_t written = 0;
        if (hda_is_present()) {
            written = hda_write_pcm(xferi->buf, total_bytes, alsa_sample_rate, alsa_channels, alsa_bits);
        } else {
            vfs_node_t *dsp = fb_lookup_device("dsp");
            if (dsp && dsp->write) {
                written = dsp->write(dsp, 0, total_bytes, (uint8_t *)xferi->buf);
            }
        }

        uint64_t frames_written = (written / frame_size);
        xferi->result = frames_written;
        alsa_appl_ptr += frames_written;
        alsa_pcm_state = 3; // SNDRV_PCM_STATE_RUNNING
        if (alsa_status_page_virt) {
            ((struct snd_pcm_mmap_status *)alsa_status_page_virt)->state = alsa_pcm_state;
        }
        if (alsa_control_page_virt) {
            ((struct snd_pcm_mmap_control *)alsa_control_page_virt)->appl_ptr = alsa_appl_ptr;
        }
        return 0;
    }

    if (request == SNDRV_PCM_IOCTL_READI_FRAMES) {
        return -22; // -EINVAL (capture not supported)
    }

    if (request == SNDRV_PCM_IOCTL_MMAP_BEGIN) {
        struct snd_pcm_mmap_begin_area *area = (struct snd_pcm_mmap_begin_area *)arg;
        if (!area) return -14;
        uint32_t frame_size = (alsa_bits / 8) * alsa_channels;
        if (frame_size == 0) frame_size = 4;
        uint32_t buf_frames = ALSA_DMA_BUF_SIZE / frame_size;
        uint32_t off = (uint32_t)(alsa_mmap_appl_offset % buf_frames);
        area->offset = off;
        area->frames = buf_frames - off; // contiguous frames available
        area->cont   = buf_frames - off;
        return 0;
    }

    if (request == SNDRV_PCM_IOCTL_MMAP_COMMIT) {
        struct snd_xferi *xferi = (struct snd_xferi *)arg;
        if (!xferi) return -14;
        uint32_t frame_size = (alsa_bits / 8) * alsa_channels;
        if (frame_size == 0) frame_size = 4;
        uint32_t buf_frames = ALSA_DMA_BUF_SIZE / frame_size;
        uint64_t frames = xferi->frames;
        if (frames == 0) { xferi->result = 0; return 0; }
        if (frames > buf_frames) frames = buf_frames;
        uint32_t off = (uint32_t)(alsa_mmap_appl_offset % buf_frames);
        uint32_t byte_off = off * frame_size;
        uint32_t byte_len = (uint32_t)(frames * frame_size);
        // Flush mmap buffer to hardware
        if (alsa_dma_buf_virt) {
            uint8_t *src = alsa_dma_buf_virt + byte_off;
            if (hda_is_present()) {
                hda_write_pcm(src, byte_len, alsa_sample_rate, alsa_channels, alsa_bits);
            } else {
                vfs_node_t *dsp = fb_lookup_device("dsp");
                if (dsp && dsp->write) {
                    dsp->write(dsp, 0, byte_len, src);
                }
            }
        }
        alsa_mmap_appl_offset += frames;
        alsa_appl_ptr += frames;
        alsa_pcm_state = 3; // SNDRV_PCM_STATE_RUNNING
        xferi->result = (int64_t)frames;
        if (alsa_status_page_virt) {
            ((struct snd_pcm_mmap_status *)alsa_status_page_virt)->state = alsa_pcm_state;
        }
        if (alsa_control_page_virt) {
            ((struct snd_pcm_mmap_control *)alsa_control_page_virt)->appl_ptr = alsa_appl_ptr;
        }
        return 0;
    }

    if (request == SNDRV_PCM_IOCTL_LINK || request == SNDRV_PCM_IOCTL_UNLINK) {
        return 0;
    }

    if (request == SNDRV_PCM_IOCTL_STATUS_EXT || request == SNDRV_PCM_IOCTL_STATUS) {
        // Extended/standard status
        struct snd_pcm_status *st = (struct snd_pcm_status *)arg;
        if (!st) return -14;
        memset(st, 0, sizeof(*st));
        st->state = alsa_pcm_state;
        uint32_t frame_size = (alsa_bits / 8) * alsa_channels;
        if (frame_size == 0) frame_size = 4;
        uint32_t queued_bytes = hda_is_present() ? hda_get_ring_count() : 0;
        uint64_t queued_frames = queued_bytes / frame_size;
        if (queued_frames > alsa_appl_ptr) queued_frames = alsa_appl_ptr;
        st->appl_ptr  = alsa_appl_ptr;
        st->hw_ptr    = alsa_appl_ptr - queued_frames;
        st->delay     = (int64_t)queued_frames;
        uint32_t avail_bytes = (ALSA_DMA_BUF_SIZE > queued_bytes) ? (ALSA_DMA_BUF_SIZE - queued_bytes) : 0;
        st->avail     = avail_bytes / frame_size;
        st->avail_max = ALSA_DMA_BUF_SIZE / frame_size;
        return 0;
    }

    if (request == SNDRV_PCM_IOCTL_RESUME || request == SNDRV_PCM_IOCTL_XRUN) {
        return 0;
    }

    if (request == SNDRV_PCM_IOCTL_REWIND || request == SNDRV_PCM_IOCTL_FORWARD) {
        // Rewind/forward pointer — just acknowledge
        return 0;
    }

    if (request == SNDRV_PCM_IOCTL_WRITEN_FRAMES || request == SNDRV_PCM_IOCTL_READN_FRAMES) {
        return -22; // -EINVAL (non-interleaved not supported)
    }

    return -25; // -ENOTTY (Inappropriate ioctl for device)
}

static int alsa_pcm_ioctl(struct vfs_node *node, uint32_t request, uint64_t arg) {
    int ret = alsa_pcm_ioctl_internal(node, request, arg);
    return ret;
}

static uint32_t alsa_pcm_write(struct vfs_node *node, uint32_t offset,
                              uint32_t size, uint8_t *buffer) {
    (void)node;
    (void)offset;
    if (hda_is_present()) {
        return hda_write_pcm(buffer, size, alsa_sample_rate, alsa_channels, alsa_bits);
    }
    vfs_node_t *dsp = fb_lookup_device("dsp");
    if (dsp && dsp->write) {
        return dsp->write(dsp, offset, size, buffer);
    }
    return size;
}

static int alsa_pcm_poll(struct vfs_node *node, int events) {
    (void)node;
    if (hda_is_present()) {
        return hda_poll_handler(events);
    }
    return (events & (POLLOUT | POLLWRNORM));
}

// ALSA Timer Callbacks for /dev/snd/timer
static int alsa_timer_ioctl_internal(struct vfs_node *node, uint32_t request, uint64_t arg) {
    (void)node;

    // SNDRV_TIMER_IOCTL_PVERSION (0x80045400)
    if (request == 0x80045400 || (request & 0xFFFF) == 0x5400) {
        int *ver = (int *)arg;
        if (ver) *ver = 0x00020006; // Timer protocol 2.0.6
        return 0;
    }

    // SNDRV_TIMER_IOCTL_NEXT_DEVICE (0xC0145401)
    if (request == 0xC0145401 || (request & 0xFFFF) == 0x5401) {
        return 0;
    }

    // SNDRV_TIMER_IOCTL_TREAD (0x40045402)
    if (request == 0x40045402 || (request & 0xFFFF) == 0x5402) {
        return 0;
    }

    // SNDRV_TIMER_IOCTL_SELECT (0x40145410)
    if (request == 0x40145410 || (request & 0xFFFF) == 0x5410) {
        return 0;
    }

    // SNDRV_TIMER_IOCTL_INFO (0x80e85411)
    if (request == 0x80E85411 || (request & 0xFFFF) == 0x5411) {
        struct {
            unsigned int flags;
            int card;
            uint8_t id[64];
            uint8_t name[80];
            uint64_t reserved0;
            uint64_t resolution;
            uint8_t reserved[64];
        } *info = (void *)arg;
        if (info) {
            memset(info, 0, sizeof(*info));
            info->card = 0;
            strcpy((char *)info->id, "PCM timer");
            strcpy((char *)info->name, "HDA PCM timer");
            info->resolution = 1000000; // 1 ms
        }
        return 0;
    }

    // SNDRV_TIMER_IOCTL_PARAMS (0x40205412)
    if (request == 0x40205412 || (request & 0xFFFF) == 0x5412) {
        return 0;
    }

    // SNDRV_TIMER_IOCTL_STATUS (0x80385414)
    if (request == 0x80385414 || (request & 0xFFFF) == 0x5414) {
        struct {
            uint64_t tv_sec;
            uint64_t tv_nsec;
            unsigned int resolution;
            unsigned int lost;
            unsigned int overrange;
            unsigned int queue;
            uint8_t reserved[64];
        } *st = (void *)arg;
        if (st) {
            memset(st, 0, sizeof(*st));
            st->resolution = 1000000;
        }
        return 0;
    }

    // SNDRV_TIMER_IOCTL_START (0x54a0), STOP (0x54a1), CONTINUE (0x54a2), PAUSE (0x54a3)
    if ((request & 0xFFF0) == 0x54A0 || request == 0x54A0 || request == 0x54A1 ||
        request == 0x54A2 || request == 0x54A3) {
        return 0;
    }

    return 0; // Accept any other timer ioctls safely
}

static int alsa_timer_ioctl(struct vfs_node *node, uint32_t request, uint64_t arg) {
    return alsa_timer_ioctl_internal(node, request, arg);
}

static uint32_t alsa_timer_read(struct vfs_node *node, uint32_t offset, uint32_t size, uint8_t *buffer) {
    (void)node; (void)offset;
    if (buffer && size >= sizeof(unsigned long)) {
        unsigned long ticks = 1;
        memcpy(buffer, &ticks, sizeof(ticks));
        return sizeof(ticks);
    }
    return size;
}

static int alsa_timer_poll(struct vfs_node *node, int events) {
    (void)node;
    return (events & (POLLIN | POLLRDNORM | POLLOUT | POLLWRNORM));
}

// Map the PCM DMA ring buffer into the calling process's address space.
// ALSA's hw plugin mmap()s three distinct areas using special page offsets:
//   SNDRV_PCM_MMAP_OFFSET_STATUS  = 0x80000000 -> snd_pcm_mmap_status  (read-only)
//   SNDRV_PCM_MMAP_OFFSET_CONTROL = 0x81000000 -> snd_pcm_mmap_control (read-write)
//   SNDRV_PCM_MMAP_OFFSET_DATA    = 0x00000000 -> DMA audio ring buffer
static uint64_t alsa_pcm_mmap(struct vfs_node *node, uint64_t addr,
                               uint64_t length, uint64_t prot,
                               uint64_t flags, uint64_t offset) {
    (void)node;
    (void)flags;

    uint64_t *pml4 = vmm_get_active_pml4();
    if (!pml4) {
        return (uint64_t)-1;
    }

    uint64_t page_flags = PAGE_FLAG_PRESENT | PAGE_FLAG_USER;
    if (prot & 0x2 /* PROT_WRITE */) page_flags |= PAGE_FLAG_RW;

    uint64_t phys_base;
    uint64_t map_size;

    if (offset >= SNDRV_PCM_MMAP_OFFSET_CONTROL) {
        // Control page: the hw plugin writes appl_ptr/avail_min here
        if (!alsa_control_page_phys) { return (uint64_t)-1; }
        phys_base = alsa_control_page_phys;
        map_size  = 4096;
    } else if (offset >= SNDRV_PCM_MMAP_OFFSET_STATUS) {
        // Status page: the hw plugin reads state/hw_ptr from here
        if (!alsa_status_page_phys) { return (uint64_t)-1; }
        phys_base = alsa_status_page_phys;
        map_size  = 4096;
        // Reflect current state into the status page
        struct snd_pcm_mmap_status *st = (struct snd_pcm_mmap_status *)alsa_status_page_virt;
        if (st) {
            st->state  = alsa_pcm_state;
            st->hw_ptr = alsa_hw_ptr;
        }
    } else {
        // DATA area: the audio ring buffer
        if (!alsa_dma_buf_virt || !alsa_dma_buf_phys) { return (uint64_t)-1; }
        phys_base = alsa_dma_buf_phys;
        map_size  = ALSA_DMA_BUF_SIZE;
        if (length < map_size) map_size = length;
    }

    uint64_t pages = (map_size + 4095) / 4096;
    for (uint64_t i = 0; i < pages; i++) {
        vmm_map_page(pml4, addr + i * 4096, phys_base + i * 4096, page_flags);
    }
    return addr;
}

void alsa_emu_init(void) {
    alsa_emu_register_vfs();
}

void alsa_emu_register_vfs(void) {
    vfs_node_t *snd_dir = NULL;
    vfs_node_t *dev_dir = vfs_resolve_path("/dev");
    if (dev_dir) {
        if (dev_dir->mkdir) {
            dev_dir->mkdir(dev_dir, "snd", 0755);
        }
        snd_dir = vfs_resolve_path("/dev/snd");
    }

    // Allocate the PCM DMA ring buffer (contiguous physical pages)
    if (!alsa_dma_buf_virt) {
        void *phys = pmm_alloc_pages(ALSA_DMA_BUF_PAGES);
        if (phys) {
            alsa_dma_buf_phys = (uint64_t)phys;
            // Map into kernel space via HHDM
            alsa_dma_buf_virt = (uint8_t *)(alsa_dma_buf_phys + pmm_get_hhdm_offset());
            // Zero it out
            memset(alsa_dma_buf_virt, 0, ALSA_DMA_BUF_SIZE);
        }
    }

    // Allocate the STATUS page (read-only mmap at offset 0x80000000)
    if (!alsa_status_page_virt) {
        void *phys = pmm_alloc_page();
        if (phys) {
            alsa_status_page_phys = (uint64_t)phys;
            alsa_status_page_virt = (uint8_t *)(alsa_status_page_phys + pmm_get_hhdm_offset());
            memset(alsa_status_page_virt, 0, 4096);
            // Pre-fill state = PREPARED (2)
            struct snd_pcm_mmap_status *st = (struct snd_pcm_mmap_status *)alsa_status_page_virt;
            st->state = 2;
        }
    }

    // Allocate the CONTROL page (rw mmap at offset 0x81000000)
    if (!alsa_control_page_virt) {
        void *phys = pmm_alloc_page();
        if (phys) {
            alsa_control_page_phys = (uint64_t)phys;
            alsa_control_page_virt = (uint8_t *)(alsa_control_page_phys + pmm_get_hhdm_offset());
            memset(alsa_control_page_virt, 0, 4096);
            // avail_min = 1 frame by default
            struct snd_pcm_mmap_control *ctrl = (struct snd_pcm_mmap_control *)alsa_control_page_virt;
            ctrl->avail_min = 1;
        }
    }

    // 1. Register /dev/snd/controlC0
    vfs_node_t *ctl_node = kmalloc(sizeof(vfs_node_t));
    if (ctl_node) {
        vfs_node_init(ctl_node);
        strcpy(ctl_node->name, "controlC0");
        ctl_node->flags = FS_CHARDEV | FS_PERSISTENT;
        ctl_node->mask = 0666;
        ctl_node->length = 0;
        ctl_node->ioctl = alsa_ctl_ioctl;
        ctl_node->inode = (116 << 8) | 0; // major 116, minor 0
        fb_register_device_node("snd/controlC0", ctl_node);
        fb_register_device_node("controlC0", ctl_node);
        if (snd_dir) {
            ramfs_mount_node(snd_dir, ctl_node);
        }
    }

    // 2. Register /dev/snd/pcmC0D0p
    vfs_node_t *pcm_node = kmalloc(sizeof(vfs_node_t));
    if (pcm_node) {
        vfs_node_init(pcm_node);
        strcpy(pcm_node->name, "pcmC0D0p");
        pcm_node->flags = FS_CHARDEV | FS_PERSISTENT;
        pcm_node->mask = 0666;
        pcm_node->length = ALSA_DMA_BUF_SIZE;
        pcm_node->write = alsa_pcm_write;
        pcm_node->ioctl = alsa_pcm_ioctl;
        pcm_node->poll  = alsa_pcm_poll;
        pcm_node->mmap  = alsa_pcm_mmap;
        pcm_node->inode = (116 << 8) | 24; // major 116, minor 24
        if (hda_is_present()) {
            pcm_node->wait_queue = hda_get_wait_queue();
        }
        fb_register_device_node("snd/pcmC0D0p", pcm_node);
        fb_register_device_node("pcmC0D0p", pcm_node);
        if (snd_dir) {
            ramfs_mount_node(snd_dir, pcm_node);
        }
    }

    // 3. Register /dev/snd/timer
    vfs_node_t *timer_node = kmalloc(sizeof(vfs_node_t));
    if (timer_node) {
        vfs_node_init(timer_node);
        strcpy(timer_node->name, "timer");
        timer_node->flags = FS_CHARDEV | FS_PERSISTENT;
        timer_node->mask = 0666;
        timer_node->length = 0;
        timer_node->read  = alsa_timer_read;
        timer_node->ioctl = alsa_timer_ioctl;
        timer_node->poll  = alsa_timer_poll;
        timer_node->inode = (116 << 8) | 33; // major 116, minor 33
        fb_register_device_node("snd/timer", timer_node);
        fb_register_device_node("timer", timer_node);
        if (snd_dir) {
            ramfs_mount_node(snd_dir, timer_node);
        }
    }

    // 4. Register /dev/snd/seq
    vfs_node_t *seq_node = kmalloc(sizeof(vfs_node_t));
    if (seq_node) {
        vfs_node_init(seq_node);
        strcpy(seq_node->name, "seq");
        seq_node->flags = FS_CHARDEV | FS_PERSISTENT;
        seq_node->mask = 0666;
        seq_node->length = 0;
        seq_node->inode = (116 << 8) | 1; // major 116, minor 1
        fb_register_device_node("snd/seq", seq_node);
        fb_register_device_node("seq", seq_node);
        if (snd_dir) {
            ramfs_mount_node(snd_dir, seq_node);
        }
    }
}