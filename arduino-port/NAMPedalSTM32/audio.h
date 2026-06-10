#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

// ─── Guitar ADC input (ADC1/PA1, TIM3-triggered at 48 kHz, DMA) ──────────────

// Initialise ADC1 (TIM3-triggered, 48 kHz) with DMA.
// Call once before audio_adc_start().
void audio_adc_init(void);

// Start the ADC DMA.  Call after audio_adc_init().
void audio_adc_start(void);

// Block until 48 fresh ADC samples are available, then copy them into
// `out` as normalised floats (–1 … +1).  Timeout after `timeout_ms` ms;
// returns false on timeout (fills `out` with zeros), true on success.
bool audio_adc_read(float* out, size_t frames, uint32_t timeout_ms);

// ─── PCM5102A I2S output (I2S2: PB12=LRCK, PB13=BCK, PB15=DIN, DMA) ──────────

// Configure PLLI2S (exact 48 kHz), I2S2 GPIO/peripheral and circular DMA.
// Call once before audio_i2s_start().
void audio_i2s_init(void);

// Start the circular I2S TX DMA (outputs silence until written to).
void audio_i2s_start(void);

// Write one block of interleaved stereo 16-bit samples (frames × 2 values).
// Blocks until a DMA half-buffer is free (≤ ~1 ms), then copies into it.
// Returns false on timeout (~5 ms — indicates I2S DMA is not running).
bool audio_i2s_write(const int16_t* stereo, size_t frames);
