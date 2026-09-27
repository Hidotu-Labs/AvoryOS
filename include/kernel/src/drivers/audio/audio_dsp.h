#ifndef AUDIO_DSP_H
#define AUDIO_DSP_H

#include <stdbool.h>
#include <stdint.h>

// Audio DSP dispatcher - routes /dev/dsp to available audio hardware
// Priority: AC97 (PCI) > SB16 (ISA)

void audio_dsp_register_vfs(void);

#endif
