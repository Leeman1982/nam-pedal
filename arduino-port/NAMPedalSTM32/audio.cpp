// audio.cpp — Guitar ADC input driver (STM32F405).
//
// ADC1 CH1 (PA1) is triggered by TIM3 at 48 kHz.
// DMA2 Stream0 CH0 collects AUDIO_BLOCK_FRAMES samples per block.
// When the DMA completes: samples are snapshotted into adc_snapshot[],
// the ready flag is set, and the DMA is restarted for the next block.
// The I2S output is handled entirely by Arduino Audio Tools (I2SStream).
//
// Clock: TIM3 on APB1, timer clock = 84 MHz → period = 84000000/48000-1 = 1749.

#include "audio.h"
#include "config.h"
#include <Arduino.h>
#include <stm32f4xx_hal.h>
#include <string.h>

// ─── ADC DMA buffer — must NOT be in CCM (0x10000000) on STM32F4 ─────────────
static uint16_t adc_buf[AUDIO_BLOCK_FRAMES];          // DMA target
static uint16_t adc_snapshot[AUDIO_BLOCK_FRAMES];     // safe copy for main loop
static volatile bool adc_block_ready = false;

static ADC_HandleTypeDef hadc1;
static DMA_HandleTypeDef hdma_adc1;
static TIM_HandleTypeDef htim3;

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

// ─── Initialise ──────────────────────────────────────────────────────────────

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
