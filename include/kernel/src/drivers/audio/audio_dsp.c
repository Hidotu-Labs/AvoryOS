#include "audio_dsp.h"
#include "../../fb/framebuffer.h"
#include "../../fs/vfs.h"
#include "../../lib/string.h"
#include "../../mm/heap.h"
#include "ac97.h"
#include "hda.h"
#include "sb16.h"
#include <stdbool.h>
#include <stdint.h>

// Audio device priority: HDA (Modern PCI) > AC97 (PCI) > SB16 (ISA)
static bool hda_available = false;
static bool ac97_available = false;
static bool sb16_available = false;

// OSS ioctl constants (shared between drivers)
#define SNDCTL_DSP_RESET     0x5000
#define SNDCTL_DSP_SYNC      0x5001
#define SNDCTL_DSP_SPEED     0xC0045002
#define SNDCTL_DSP_STEREO    0xC0045003
#define SNDCTL_DSP_GETBLKSIZE 0xC0045004
#define SNDCTL_DSP_SETFMT    0xC0045005
#define SNDCTL_DSP_CHANNELS  0xC0045006
#define SNDCTL_DSP_POST      0x5008
#define SNDCTL_DSP_SUBDIVIDE 0xC0045009
#define SNDCTL_DSP_SETFRAGMENT 0xC004500A
#define SNDCTL_DSP_NONBLOCK  0x500B
#define SNDCTL_DSP_GETOSPACE 0x8010500C
#define SNDCTL_DSP_GETODELAY 0x80044D1D
#define OSS_GETVERSION       0x80044D76

#define SOUND_MIXER_READ_VOLUME     0x80044D00
#define SOUND_MIXER_WRITE_VOLUME    0xC0044D00
#define SOUND_MIXER_READ_PCM        0x80044D04
#define SOUND_MIXER_WRITE_PCM       0xC0044D04
#define SOUND_MIXER_READ_DEVMASK    0x80044DFF
#define SOUND_MIXER_READ_RECMASK    0x80044DFE
#define SOUND_MIXER_READ_STEREODEVS 0x80044DFD

#define AFMT_U8     0x00000008
#define AFMT_S16_LE 0x00000010

// Check which audio devices are available
void audio_dsp_init(void) {
  vfs_node_t *hda_node = fb_lookup_device("hda_audio");
  vfs_node_t *ac97_node = fb_lookup_device("ac97");
  vfs_node_t *sb16_node = fb_lookup_device("sb16");

  hda_available = (hda_node != NULL);
  ac97_available = (ac97_node != NULL);
  sb16_available = (sb16_node != NULL);
}

// Get the active audio device's VFS node
static vfs_node_t *get_active_audio_node(void) {
  // Prefer HDA (Modern PCI) > AC97 (PCI) > SB16 (ISA)
  if (hda_available) {
    return fb_lookup_device("hda_audio");
  }
  if (ac97_available) {
    return fb_lookup_device("ac97");
  }
  if (sb16_available) {
    return fb_lookup_device("sb16");
  }
  return NULL;
}

// Dispatch write to active audio device
static uint32_t dsp_vfs_write(struct vfs_node *node, uint32_t offset,
                              uint32_t size, uint8_t *buffer) {
  (void)node;
  vfs_node_t *audio = get_active_audio_node();
  if (!audio || !audio->write) {
    return 0;
  }
  return audio->write(audio, offset, size, buffer);
}

// Dispatch ioctl to active audio device
static int dsp_vfs_ioctl(struct vfs_node *node, uint32_t request,
                         uint64_t arg) {
  (void)node;

  // Handle OSS version request centrally
  if (request == OSS_GETVERSION) {
    int *version = (int *)arg;
    if (!version) return -14; // EFAULT
    *version = 0x040000; // Report OSS 4.0
    return 0;
  }

  // Handle mixer queries centrally if not handled by underlying driver
  if (request == SOUND_MIXER_READ_VOLUME || request == SOUND_MIXER_READ_PCM ||
      request == SOUND_MIXER_WRITE_VOLUME || request == SOUND_MIXER_WRITE_PCM) {
    int *vol = (int *)arg;
    if (vol) *vol = 0x6464; // 100% Left / 100% Right
    return 0;
  }

  if (request == SOUND_MIXER_READ_DEVMASK || request == SOUND_MIXER_READ_RECMASK ||
      request == SOUND_MIXER_READ_STEREODEVS) {
    int *mask = (int *)arg;
    if (mask) *mask = 0x11; // Volume + PCM
    return 0;
  }

  if (request == 0x8004500B) { // SNDCTL_DSP_GETFMTS
    int *mask = (int *)arg;
    if (!mask) return -14;
    *mask = AFMT_S16_LE | AFMT_U8 | 0x00000020 | 0x00001000 | 0x00000040 | 0x00000080;
    return 0;
  }

  if (request == 0x8004500F) { // SNDCTL_DSP_GETCAPS
    int *caps = (int *)arg;
    if (!caps) return -14;
    *caps = 0x00001000 | 0x00000100 | 0x00000200 | 0x00020000 | 0x00010000; // TRIGGER | DUPLEX | REALTIME | OUTPUT | INPUT
    return 0;
  }

  if (request == SNDCTL_DSP_GETBLKSIZE || request == 0x80045004) {
    int *blksize = (int *)arg;
    if (!blksize) return -14;
    *blksize = 4096;
    return 0;
  }

  vfs_node_t *audio = get_active_audio_node();
  if (!audio || !audio->ioctl) {
    return -6; // ENXIO - no such device
  }
  return audio->ioctl(audio, request, arg);
}

// Dispatch poll to active audio device
static int dsp_vfs_poll(struct vfs_node *node, int events) {
  (void)node;
  vfs_node_t *audio = get_active_audio_node();
  if (!audio || !audio->poll) {
    return (events & (POLLOUT | POLLWRNORM));
  }
  return audio->poll(audio, events);
}

// Register /dev/dsp, /dev/audio, and /dev/mixer as dispatchers
void audio_dsp_register_vfs(void) {
  // Detect available audio hardware
  audio_dsp_init();

  if (!hda_available && !ac97_available && !sb16_available) {
    return; // No audio hardware available
  }

  // Create dispatcher node for /dev/dsp
  vfs_node_t *dsp_node = kmalloc(sizeof(vfs_node_t));
  if (dsp_node) {
    vfs_node_init(dsp_node);
    strcpy(dsp_node->name, "dsp");
    dsp_node->flags = FS_CHARDEV;
    dsp_node->mask = 0666;
    dsp_node->length = 0;
    dsp_node->write = dsp_vfs_write;
    dsp_node->ioctl = dsp_vfs_ioctl;
    dsp_node->poll = dsp_vfs_poll;
    if (hda_available) {
      dsp_node->wait_queue = hda_get_wait_queue();
    }
    fb_register_device_node("dsp", dsp_node);
  }

  // Create dispatcher node for /dev/audio
  vfs_node_t *audio_node = kmalloc(sizeof(vfs_node_t));
  if (audio_node) {
    vfs_node_init(audio_node);
    strcpy(audio_node->name, "audio");
    audio_node->flags = FS_CHARDEV;
    audio_node->mask = 0666;
    audio_node->length = 0;
    audio_node->write = dsp_vfs_write;
    audio_node->ioctl = dsp_vfs_ioctl;
    audio_node->poll = dsp_vfs_poll;
    if (hda_available) {
      audio_node->wait_queue = hda_get_wait_queue();
    }
    fb_register_device_node("audio", audio_node);
  }

  // Create dispatcher node for /dev/mixer
  vfs_node_t *mixer_node = kmalloc(sizeof(vfs_node_t));
  if (mixer_node) {
    vfs_node_init(mixer_node);
    strcpy(mixer_node->name, "mixer");
    mixer_node->flags = FS_CHARDEV;
    mixer_node->mask = 0666;
    mixer_node->length = 0;
    mixer_node->ioctl = dsp_vfs_ioctl;
    mixer_node->poll = dsp_vfs_poll;
    fb_register_device_node("mixer", mixer_node);
  }
}
