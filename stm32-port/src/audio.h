#pragma once
#include <stddef.h>
#include <stdint.h>

// Callback type: called from DMA ISR once per audio block.
// `in`  — AUDIO_BLOCK_FRAMES mono float samples from ADC (normalised –1 … +1)
// `out` — caller must fill AUDIO_BLOCK_FRAMES mono float samples
typedef void (*AudioCallback)(const float* in, float* out, size_t frames);

// Initialise ADC1 (TIM3-triggered), I2S2 TX, and their DMA streams.
// Must be called once before audio_start().
void audio_init(void);

// Register the processing callback and start both DMA streams.
void audio_start(AudioCallback cb);

// Returns true if the last audio block processing overran its deadline.
bool audio_overrun(void);
