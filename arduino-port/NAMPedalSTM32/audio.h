#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

// Initialise ADC1 (TIM3-triggered, 48 kHz) with DMA.
// Call once before audio_start().
void audio_adc_init(void);

// Start the ADC DMA.  Call after audio_adc_init().
void audio_adc_start(void);

// Block until 48 fresh ADC samples are available, then copy them into
// `out` as normalised floats (–1 … +1).  Timeout after `timeout_ms` ms;
// returns false on timeout (fills `out` with zeros), true on success.
bool audio_adc_read(float* out, size_t frames, uint32_t timeout_ms);
