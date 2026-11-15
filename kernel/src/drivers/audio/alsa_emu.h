#ifndef AUDIO_ALSA_EMU_H
#define AUDIO_ALSA_EMU_H

#include <stdbool.h>
#include <stdint.h>

struct alsa_pcm_backend_ops {
    int (*open)(void *ctx, uint32_t rate, uint8_t channels, uint8_t bits);
    uint32_t (*write)(void *ctx, const void *samples, uint32_t frames);
    uint64_t (*pointer)(void *ctx); /* monotonic completed playback frames */
    bool (*start)(void *ctx);
    bool (*drain)(void *ctx); /* submit a short final packet if needed */
    bool (*stop)(void *ctx); /* true only after DMA ownership is revoked */
    void (*close)(void *ctx);
    void *(*wait_queue)(void *ctx);
};

/* Registers a dynamic playback card. Returns its ALSA card index or -1.
 * The backend context remains owned by the caller until unregister succeeds. */
int alsa_emu_register_pcm_card(const char *name, void *ctx,
                               const struct alsa_pcm_backend_ops *ops);
bool alsa_emu_unregister_pcm_card(int card_index);
void alsa_emu_pcm_tick(void);
void alsa_emu_pcm_notify(int card_index);

void alsa_emu_init(void);
void alsa_emu_register_vfs(void);

#endif // AUDIO_ALSA_EMU_H
