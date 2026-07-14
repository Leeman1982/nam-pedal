// audio.cpp — STM32F405 audio drivers: ADC1 guitar input + I2S2 PCM5102A output.
//
// Input:  ADC1 CH1 (PA1) triggered by TIM3 at 48 kHz.
//         DMA2 Stream0 CH0 collects AUDIO_BLOCK_FRAMES samples per block
//         (single-shot, restarted in the completion callback).
//
// Output: I2S2 master TX, Philips 16-bit stereo, DMA1 Stream4 CH0 circular
//         over a double buffer.  The HAL TxHalf/TxCplt callbacks mark which
//         half just finished transmitting; audio_i2s_write() blocks until a
//         half is free and copies the next block into it.  This gives the
//         main loop natural back-pressure at exactly 48 kHz.
//
// Both clocks derive from the same 8 MHz HSE crystal, so ADC and I2S run at
// exactly 48 000 Hz with zero relative drift:
//   TIM3:   84 MHz / 1750 = 48 000 Hz
//   PLLI2S: N=192, R=5 → 38.4 MHz; HAL picks DIV=12/ODD=1 → fs = 48 000 Hz
//
// PCM5102A wiring: BCK→PB13, LRCK→PB12, DIN→PB15 (AF5 = SPI2/I2S2)
//   Strap FMT→GND (I2S), DEMP→GND, XSMT→3V3 (unmute), SCK→GND/float.

#include "audio.h"
#include "config.h"
#include <Arduino.h>
#include <stm32f4xx_hal.h>
#include <string.h>

// ─── DMA buffers — must stay in AHB SRAM (NOT CCM: 0x10000000 is CPU-only) ───

static uint16_t adc_buf[AUDIO_BLOCK_FRAMES];          // ADC DMA target
static uint16_t adc_snapshot[AUDIO_BLOCK_FRAMES];     // safe copy for main loop
static volatile bool adc_block_ready = false;

// I2S TX double buffer: 2 halves × BLOCK frames × 2 channels, interleaved.
static int16_t i2s_buf[AUDIO_BLOCK_FRAMES * 2 * 2];

// Which halves are free to write (set by DMA callbacks, cleared by writer).
static volatile bool i2s_half_free[2] = {false, false};
// Next half the writer should fill (alternates 0,1,0,1 in DMA order).
static uint8_t i2s_write_half = 0;

static ADC_HandleTypeDef hadc1;
static DMA_HandleTypeDef hdma_adc1;
static TIM_HandleTypeDef htim3;
static I2S_HandleTypeDef hi2s2;
static DMA_HandleTypeDef hdma_i2s2_tx;

// ─── ADC DMA complete — called once per block (every 1 ms) ───────────────────

extern "C" void HAL_ADC_ConvCpltCallback(ADC_HandleTypeDef* h)
{
    (void)h;
    // Copy to snapshot before restarting DMA (adc_buf will be overwritten)
    memcpy(adc_snapshot, adc_buf, sizeof(adc_buf));
    adc_block_ready = true;
    // Restart single-shot DMA for next block
    HAL_ADC_Start_DMA(&hadc1, (uint32_t*)adc_buf, AUDIO_BLOCK_FRAMES);
}

extern "C" void DMA2_Stream0_IRQHandler(void)
{
    HAL_DMA_IRQHandler(&hdma_adc1);
}

// ─── I2S DMA callbacks — half N just finished transmitting, safe to refill ───

extern "C" void HAL_I2S_TxHalfCpltCallback(I2S_HandleTypeDef* h)
{
    (void)h;
    i2s_half_free[0] = true;
}

extern "C" void HAL_I2S_TxCpltCallback(I2S_HandleTypeDef* h)
{
    (void)h;
    i2s_half_free[1] = true;
}

extern "C" void DMA1_Stream4_IRQHandler(void)   // I2S2 TX
{
    HAL_DMA_IRQHandler(&hdma_i2s2_tx);
}

extern "C" void SPI2_IRQHandler(void)           // I2S2 peripheral errors
{
    HAL_I2S_IRQHandler(&hi2s2);
}

// ─── ADC init ─────────────────────────────────────────────────────────────────

void audio_adc_init(void)
{
    __HAL_RCC_ADC1_CLK_ENABLE();
    __HAL_RCC_DMA2_CLK_ENABLE();
    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_TIM3_CLK_ENABLE();

    // PA1 as analog input
    GPIO_InitTypeDef gin = {};
    gin.Pin  = GPIO_PIN_1;
    gin.Mode = GPIO_MODE_ANALOG;
    gin.Pull = GPIO_NOPULL;
    HAL_GPIO_Init(GPIOA, &gin);

    // TIM3 at 48 kHz (timer clock = 84 MHz, period = 1749)
    htim3.Instance               = TIM3;
    htim3.Init.Prescaler         = 0;
    htim3.Init.CounterMode       = TIM_COUNTERMODE_UP;
    htim3.Init.Period            = (84000000U / AUDIO_SAMPLE_RATE) - 1U;
    htim3.Init.ClockDivision     = TIM_CLOCKDIVISION_DIV1;
    htim3.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;
    HAL_TIM_Base_Init(&htim3);

    TIM_MasterConfigTypeDef mc = {};
    mc.MasterOutputTrigger = TIM_TRGO_UPDATE;
    mc.MasterSlaveMode     = TIM_MASTERSLAVEMODE_DISABLE;
    HAL_TIMEx_MasterConfigSynchronization(&htim3, &mc);

    // DMA2 Stream0 CH0 for ADC1 (single-shot, not circular — restarted in callback)
    hdma_adc1.Instance                 = DMA2_Stream0;
    hdma_adc1.Init.Channel             = DMA_CHANNEL_0;
    hdma_adc1.Init.Direction           = DMA_PERIPH_TO_MEMORY;
    hdma_adc1.Init.PeriphInc           = DMA_PINC_DISABLE;
    hdma_adc1.Init.MemInc              = DMA_MINC_ENABLE;
    hdma_adc1.Init.PeriphDataAlignment = DMA_PDATAALIGN_HALFWORD;
    hdma_adc1.Init.MemDataAlignment    = DMA_MDATAALIGN_HALFWORD;
    hdma_adc1.Init.Mode                = DMA_NORMAL;      // single-shot
    hdma_adc1.Init.Priority            = DMA_PRIORITY_HIGH;
    hdma_adc1.Init.FIFOMode            = DMA_FIFOMODE_DISABLE;
    HAL_DMA_Init(&hdma_adc1);
    __HAL_LINKDMA(&hadc1, DMA_Handle, hdma_adc1);

    HAL_NVIC_SetPriority(DMA2_Stream0_IRQn, 3, 0);
    HAL_NVIC_EnableIRQ(DMA2_Stream0_IRQn);

    // ADC1: 12-bit, triggered by TIM3 TRGO
    hadc1.Instance                   = ADC1;
    hadc1.Init.ClockPrescaler        = ADC_CLOCK_SYNC_PCLK_DIV4;  // 21 MHz
    hadc1.Init.Resolution            = ADC_RESOLUTION_12B;
    hadc1.Init.ScanConvMode          = DISABLE;
    hadc1.Init.ContinuousConvMode    = DISABLE;
    hadc1.Init.DiscontinuousConvMode = DISABLE;
    hadc1.Init.ExternalTrigConvEdge  = ADC_EXTERNALTRIGCONVEDGE_RISING;
    hadc1.Init.ExternalTrigConv      = ADC_EXTERNALTRIGCONV_T3_TRGO;
    hadc1.Init.DataAlign             = ADC_DATAALIGN_RIGHT;
    hadc1.Init.NbrOfConversion       = 1;
    hadc1.Init.DMAContinuousRequests = DISABLE;  // single-shot per block
    hadc1.Init.EOCSelection          = ADC_EOC_SEQ_CONV;
    HAL_ADC_Init(&hadc1);

    ADC_ChannelConfTypeDef ch = {};
    ch.Channel      = AUDIO_ADC_CHANNEL;
    ch.Rank         = 1;
    ch.SamplingTime = ADC_SAMPLETIME_15CYCLES;
    HAL_ADC_ConfigChannel(&hadc1, &ch);
}

void audio_adc_start(void)
{
    // Kick off first DMA block, then start TIM3 trigger
    HAL_ADC_Start_DMA(&hadc1, (uint32_t*)adc_buf, AUDIO_BLOCK_FRAMES);
    HAL_TIM_Base_Start(&htim3);
}

bool audio_adc_read(float* out, size_t frames, uint32_t timeout_ms)
{
    uint32_t deadline = millis() + timeout_ms;
    while (!adc_block_ready)
    {
        if (millis() > deadline)
        {
            memset(out, 0, frames * sizeof(float));
            return false;
        }
    }
    adc_block_ready = false;

    // Convert 12-bit (0–4095, biased to 1.65 V mid-rail) → float –1 … +1
    for (size_t i = 0; i < frames; i++)
        out[i] = ((float)adc_snapshot[i] - 2048.0f) * (1.0f / 2048.0f);

    return true;
}

// ─── I2S2 init — PCM5102A, Philips 16-bit stereo master TX ───────────────────

void audio_i2s_init(void)
{
    // PLLI2S for exact 48 kHz (HSE = 8 MHz, PLLM = 8 → VCO-in = 1 MHz):
    //   N = 192, R = 5 → I2SCLK = 38.4 MHz
    //   HAL picks I2S prescaler DIV=12, ODD=1 → fs = 48 000 Hz exactly
    RCC_PeriphCLKInitTypeDef pclk = {};
    pclk.PeriphClockSelection = RCC_PERIPHCLK_I2S;
    pclk.PLLI2S.PLLI2SN       = 192;
    pclk.PLLI2S.PLLI2SR       = 5;
    HAL_RCCEx_PeriphCLKConfig(&pclk);

    __HAL_RCC_SPI2_CLK_ENABLE();
    __HAL_RCC_DMA1_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();

    // PB12=WS (LRCK), PB13=CK (BCK), PB15=SD (DIN to PCM5102A) — AF5
    GPIO_InitTypeDef gin = {};
    gin.Pin       = GPIO_PIN_12 | GPIO_PIN_13 | GPIO_PIN_15;
    gin.Mode      = GPIO_MODE_AF_PP;
    gin.Pull      = GPIO_NOPULL;
    gin.Speed     = GPIO_SPEED_FREQ_HIGH;
    gin.Alternate = GPIO_AF5_SPI2;
    HAL_GPIO_Init(GPIOB, &gin);

    // DMA1 Stream4 CH0 — I2S2 TX, circular over the double buffer
    hdma_i2s2_tx.Instance                 = DMA1_Stream4;
    hdma_i2s2_tx.Init.Channel             = DMA_CHANNEL_0;
    hdma_i2s2_tx.Init.Direction           = DMA_MEMORY_TO_PERIPH;
    hdma_i2s2_tx.Init.PeriphInc           = DMA_PINC_DISABLE;
    hdma_i2s2_tx.Init.MemInc              = DMA_MINC_ENABLE;
    hdma_i2s2_tx.Init.PeriphDataAlignment = DMA_PDATAALIGN_HALFWORD;
    hdma_i2s2_tx.Init.MemDataAlignment    = DMA_MDATAALIGN_HALFWORD;
    hdma_i2s2_tx.Init.Mode                = DMA_CIRCULAR;
    hdma_i2s2_tx.Init.Priority            = DMA_PRIORITY_VERY_HIGH;
    hdma_i2s2_tx.Init.FIFOMode            = DMA_FIFOMODE_DISABLE;
    HAL_DMA_Init(&hdma_i2s2_tx);
    __HAL_LINKDMA(&hi2s2, hdmatx, hdma_i2s2_tx);

    HAL_NVIC_SetPriority(DMA1_Stream4_IRQn, 1, 0);
    HAL_NVIC_EnableIRQ(DMA1_Stream4_IRQn);

    HAL_NVIC_SetPriority(SPI2_IRQn, 1, 1);
    HAL_NVIC_EnableIRQ(SPI2_IRQn);

    hi2s2.Instance            = SPI2;
    hi2s2.Init.Mode           = I2S_MODE_MASTER_TX;
    hi2s2.Init.Standard       = I2S_STANDARD_PHILIPS;
    hi2s2.Init.DataFormat     = I2S_DATAFORMAT_16B;
    hi2s2.Init.MCLKOutput     = I2S_MCLKOUTPUT_DISABLE;
    hi2s2.Init.AudioFreq      = I2S_AUDIOFREQ_48K;
    hi2s2.Init.CPOL           = I2S_CPOL_LOW;
    hi2s2.Init.ClockSource    = I2S_CLOCK_PLL;
    hi2s2.Init.FullDuplexMode = I2S_FULLDUPLEXMODE_DISABLE;
    HAL_I2S_Init(&hi2s2);

    memset(i2s_buf, 0, sizeof(i2s_buf));
}

void audio_i2s_start(void)
{
    i2s_half_free[0] = false;
    i2s_half_free[1] = false;
    i2s_write_half   = 0;
    // Circular DMA over the whole double buffer: BLOCK × 2ch × 2 halves halfwords
    HAL_I2S_Transmit_DMA(&hi2s2, (uint16_t*)i2s_buf, AUDIO_BLOCK_FRAMES * 2 * 2);
}

bool audio_i2s_write(const int16_t* stereo, size_t frames)
{
    uint8_t h = i2s_write_half;

    // Wait for half h to finish transmitting (≤ ~1 ms in steady state)
    uint32_t deadline = millis() + 5;
    while (!i2s_half_free[h])
    {
        if (millis() > deadline)
            return false;   // DMA not running — don't hang the main loop
    }
    i2s_half_free[h] = false;
    i2s_write_half   = h ^ 1;

    memcpy(&i2s_buf[h * AUDIO_BLOCK_FRAMES * 2], stereo,
           frames * 2 * sizeof(int16_t));
    return true;
}
