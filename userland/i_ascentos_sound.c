// i_ascentos_sound.c — AscentOS sound backend for doomgeneric
// Cleaned up version supporting only SFX (monster sounds) for now.

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include "doomtype.h"
#include "i_sound.h"
#include "w_wad.h"
#include "z_zone.h"

// ── OSS /dev/dsp ioctls ────────────────────────────────────────────────────
#define SNDCTL_DSP_SPEED 0xC0045002
#define SNDCTL_DSP_STEREO 0xC0045003
#define SNDCTL_DSP_SETFMT 0xC0045005
#define SNDCTL_DSP_CHANNELS 0xC0045006
#define SNDCTL_DSP_SETFRAGMENT 0xC004500A
#define AFMT_S16_LE 0x00000010

// ── Configuration ────────────────────────────────────────────────────────────
#define MAX_CHANNELS 8
#define OUTPUT_SAMPLERATE 48000
#define OUTPUT_CHANNELS 2
#define MIX_BUFFER_SIZE 2048

// ── Sound state ─────────────────────────────────────────────────────────────
static int dsp_fd = -1;
static uint64_t last_mix_time = 0;

typedef struct {
  uint8_t *data;
  uint32_t length;
  uint32_t sample_rate;
  int playing;
  uint32_t pos_frac; // 16.16 fixed point
  int volume;        // 0-127
  int sep;           // 0-254
} channel_t;

static channel_t channels[MAX_CHANNELS];

// ── SFX Functions ────────────────────────────────────────────────────────────

static uint64_t get_time_us(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000ULL + ts.tv_nsec / 1000;
}

static boolean I_AscentOS_InitSound(boolean use_sfx_prefix) {
  (void)use_sfx_prefix;

  dsp_fd = open("/dev/dsp", O_WRONLY | O_NONBLOCK);
  if (dsp_fd < 0) {
    printf("I_InitSound: Cannot open /dev/dsp\n");
    return false;
  }

  // Set fragment size: 4 fragments of 2^10 = 1024 bytes (512 samples)
  int frag = 0x0004000A;
  ioctl(dsp_fd, SNDCTL_DSP_SETFRAGMENT, &frag);

  int fmt = AFMT_S16_LE;
  int num_ch = OUTPUT_CHANNELS;
  int rate = OUTPUT_SAMPLERATE;

  ioctl(dsp_fd, SNDCTL_DSP_SETFMT, &fmt);
  ioctl(dsp_fd, SNDCTL_DSP_CHANNELS, &num_ch);
  ioctl(dsp_fd, SNDCTL_DSP_SPEED, &rate);

  memset(channels, 0, sizeof(channels));
  last_mix_time = get_time_us();
  printf("I_InitSound: /dev/dsp initialized at %d Hz, 16-bit Stereo\n", rate);

  return true;
}

static void I_AscentOS_ShutdownSound(void) {
  if (dsp_fd >= 0) {
    close(dsp_fd);
    dsp_fd = -1;
  }
}

static int I_AscentOS_GetSfxLumpNum(sfxinfo_t *sfxinfo) {
  char namebuf[9];
  snprintf(namebuf, sizeof(namebuf), "ds%s", sfxinfo->name);
  return W_GetNumForName(namebuf);
}

static void I_AscentOS_UpdateSound(void) {
  if (dsp_fd < 0)
    return;

  uint64_t now = get_time_us();
  uint64_t delta_us = now - last_mix_time;
  int samples_to_mix = (delta_us * OUTPUT_SAMPLERATE) / 1000000;

  if (samples_to_mix <= 0)
    return;
  if (samples_to_mix > MIX_BUFFER_SIZE)
    samples_to_mix = MIX_BUFFER_SIZE;

  int16_t mix_buffer[MIX_BUFFER_SIZE * OUTPUT_CHANNELS];
  int32_t accum[MIX_BUFFER_SIZE * OUTPUT_CHANNELS];
  memset(accum, 0, sizeof(accum));

  for (int i = 0; i < MAX_CHANNELS; i++) {
    if (!channels[i].playing)
      continue;

    uint32_t step = (channels[i].sample_rate << 16) / OUTPUT_SAMPLERATE;
    int vol = channels[i].volume;
    int sep = channels[i].sep;
    int left_scale = (vol * (254 - sep)) / 254;
    int right_scale = (vol * sep) / 254;

    for (int j = 0; j < samples_to_mix; j++) {
      uint32_t idx = channels[i].pos_frac >> 16;
      if (idx >= channels[i].length) {
        channels[i].playing = 0;
        break;
      }

      int32_t sample = (int32_t)channels[i].data[idx] - 128;
      sample <<= 8;

      accum[j * 2] += (sample * left_scale) / 64;
      accum[j * 2 + 1] += (sample * right_scale) / 64;

      channels[i].pos_frac += step;
    }
  }

  for (int i = 0; i < samples_to_mix * OUTPUT_CHANNELS; i++) {
    int32_t val = accum[i];
    if (val > 32767)
      val = 32767;
    if (val < -32768)
      val = -32768;
    mix_buffer[i] = (int16_t)val;
  }

  write(dsp_fd, mix_buffer, samples_to_mix * OUTPUT_CHANNELS * 2);
  last_mix_time = now;
}

static int I_AscentOS_StartSound(sfxinfo_t *sfxinfo, int channel, int vol,
                                 int sep) {
  if (channel < 0 || channel >= MAX_CHANNELS)
    return -1;

  int lump = I_AscentOS_GetSfxLumpNum(sfxinfo);
  if (lump < 0)
    return -1;

  int size = W_LumpLength(lump);
  if (size < 8)
    return -1;

  uint8_t *data = (uint8_t *)W_CacheLumpNum(lump, PU_STATIC);

  // Doom sound header
  channels[channel].sample_rate = data[2] | (data[3] << 8);
  channels[channel].length =
      data[4] | (data[5] << 8) | (data[6] << 16) | (data[7] << 24);
  channels[channel].data = data + 8;
  channels[channel].playing = 1;
  channels[channel].pos_frac = 0;
  channels[channel].volume = vol;
  channels[channel].sep = sep;

  return channel;
}

static void I_AscentOS_StopSound(int channel) {
  if (channel >= 0 && channel < MAX_CHANNELS)
    channels[channel].playing = 0;
}

static boolean I_AscentOS_SoundIsPlaying(int channel) {
  if (channel >= 0 && channel < MAX_CHANNELS)
    return channels[channel].playing;
  return false;
}

static void I_AscentOS_UpdateSoundParams(int channel, int vol, int sep) {
  if (channel >= 0 && channel < MAX_CHANNELS) {
    channels[channel].volume = vol;
    channels[channel].sep = sep;
  }
}

// ── Music Stubs (Music is currently disabled/broken) ─────────────────────────

void I_InitMusic(void) {}
void I_ShutdownMusic(void) {}
void I_SetMusicVolume(int vol) { (void)vol; }
void I_PauseSong(void) {}
void I_ResumeSong(void) {}
void *I_RegisterSong(void *data, int len) {
  (void)data;
  (void)len;
  return (void *)1;
}
void I_UnRegisterSong(void *handle) { (void)handle; }
void I_PlaySong(void *handle, boolean looping) {
  (void)handle;
  (void)looping;
}
void I_StopSong(void) {}
boolean I_MusicIsPlaying(void) { return false; }

// ── doomgeneric Glue ─────────────────────────────────────────────────────────

void I_InitSound(boolean use_sfx_prefix) {
  I_AscentOS_InitSound(use_sfx_prefix);
}
void I_ShutdownSound(void) { I_AscentOS_ShutdownSound(); }
void I_UpdateSound(void) { I_AscentOS_UpdateSound(); }
int I_StartSound(sfxinfo_t *sfx, int ch, int vol, int sep) {
  return I_AscentOS_StartSound(sfx, ch, vol, sep);
}
void I_StopSound(int ch) { I_AscentOS_StopSound(ch); }
boolean I_SoundIsPlaying(int ch) { return I_AscentOS_SoundIsPlaying(ch); }
void I_UpdateSoundParams(int ch, int vol, int sep) {
  I_AscentOS_UpdateSoundParams(ch, vol, sep);
}
void I_PrecacheSounds(sfxinfo_t *s, int n) {
  (void)s;
  (void)n;
}
int I_GetSfxLumpNum(sfxinfo_t *sfx) { return I_AscentOS_GetSfxLumpNum(sfx); }
void I_BindSoundVariables(void) {}

// Icons/Other stubs
int use_libsamplerate = 0;
float libsamplerate_scale = 1.0f;
int snd_musicdevice = 0;

static snddevice_t ascentos_sound_devices[] = {SNDDEVICE_SB};

sound_module_t DG_sound_module = {
    .sound_devices = ascentos_sound_devices,
    .num_sound_devices = 1,
    .Init = I_AscentOS_InitSound,
    .Shutdown = I_AscentOS_ShutdownSound,
    .GetSfxLumpNum = I_AscentOS_GetSfxLumpNum,
    .Update = I_AscentOS_UpdateSound,
    .UpdateSoundParams = I_AscentOS_UpdateSoundParams,
    .StartSound = I_AscentOS_StartSound,
    .StopSound = I_AscentOS_StopSound,
    .SoundIsPlaying = I_AscentOS_SoundIsPlaying,
    .CacheSounds = (void *)I_PrecacheSounds,
};
