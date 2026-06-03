// audio.cpp — STM32F405 audio engine: ADC1 input (TIM3-triggered DMA) + I2S2 output (DMA).
//
// Clock maths (HSE=8 MHz, PLLM=8):
//   PLLI2S: N=192, R=5 → VCO=192 MHz, I2SCLK=38.4 MHz
//   I2S2 prescaler: DIV=12, ODD=1 → BCK = 38.4/(2×12+1) = 1.536 MHz → fs = 48 000 Hz
//
// DMA:
//   ADC1   → DMA2 Stream0 CH0, circular, 16-bit halfword, buffer = 2 × AUDIO_BLOCK_FRAMES
//   I2S2TX → DMA1 Stream4 CH0, circular, 16-bit halfword, buffer = 2 × AUDIO_BLOCK_FRAMES × 2 (stereo)
//
// The ADC half/full-complete callbacks run the NAM audio processing callback,
// then convert the float output to 16-bit signed and write it into the
// corresponding half of the I2S DMA buffer.

#include "audio.h"
#include "config.h"
#include <Arduino.h>
#include <stm32f4xx_hal.h>
#include <string.h>

// ─── DMA buffers (must NOT be in CCM — CCM is not DMA-accessible on STM32F4) ──

static uint16_t adc_buf[AUDIO_BLOCK_FRAMES * 2];   // 12-bit ADC results, 2 halves

// I2S DMA buffer: interleaved stereo 16-bit signed, 2 halves × BLOCK × 2 ch
static int16_t i2s_buf[AUDIO_BLOCK_FRAMES * 2 * 2];

// Float scratch used inside the ISR callback — not DMA-mapped so CCM would be fine,
// but we keep them in normal SRAM to simplify the linker.
static float  f_in[AUDIO_BLOCK_FRAMES];
static float  f_out[AUDIO_BLOCK_FRAMES];

// ─── HAL handles ──────────────────────────────────────────────────────────────

static ADC_HandleTypeDef  hadc1;
static DMA_HandleTypeDef  hdma_adc1;
static I2S_HandleTypeDef  hi2s2;
static DMA_HandleTypeDef  hdma_i2s2_tx;
static TIM_HandleTypeDef  htim3;

static AudioCallback s_callback = nullptr;
static volatile bool s_overrun  = false;
static volatile bool s_busy     = false;   // ISR re-entrancy guard

// ─── Internal: process one half-block ─────────────────────────────────────────

static void process_half(int half)
{
    if (s_busy) { s_overrun = true; return; }
    s_busy = true;

    // Enable FPU Flush-to-Zero + Default-NaN (Cortex-M4F)
    __set_FPSCR(__get_FPSCR() | (1U << 24) | (1U << 25));

    const uint16_t* adc_half = adc_buf + half * AUDIO_BLOCK_FRAMES;
    int16_t*        i2s_half = i2s_buf + half * AUDIO_BLOCK_FRAMES * 2;

    // Convert 12-bit ADC (0–4095, biased to mid-rail) → float –1 … +1
    for (size_t i = 0; i < AUDIO_BLOCK_FRAMES; i++)
        f_in[i] = ((float)adc_half[i] - 2048.0f) * (1.0f / 2048.0f);

    memset(f_out, 0, sizeof(f_out));

    if (s_callback)
        s_callback(f_in, f_out, AUDIO_BLOCK_FRAMES);

    // Convert float → 16-bit signed stereo (L and R are identical for mono NAM)
    for (size_t i = 0; i < AUDIO_BLOCK_FRAMES; i++)
    {
        // Clamp to –1 … +1 before scaling
        float s = f_out[i];
        if (s >  1.0f) s =  1.0f;
        if (s < -1.0f) s = -1.0f;
        int16_t pcm = (int16_t)(s * 32767.0f);
        i2s_half[i * 2]     = pcm;  // L
        i2s_half[i * 2 + 1] = pcm;  // R
    }

    s_busy = false;
}

// ─── HAL callbacks (override weak symbols) ───────────────────────────────────

extern "C" void HAL_ADC_ConvHalfCpltCallback(ADC_HandleTypeDef* h)
{
    (void)h;
    process_half(0);
}

extern "C" void HAL_ADC_ConvCpltCallback(ADC_HandleTypeDef* h)
{
    (void)h;
    process_half(1);
}

// ─── DMA IRQ handlers ─────────────────────────────────────────────────────────

extern "C" void DMA2_Stream0_IRQHandler(void)
{
    HAL_DMA_IRQHandler(&hdma_adc1);
}

extern "C" void DMA1_Stream4_IRQHandler(void)
{
    HAL_DMA_IRQHandler(&hdma_i2s2_tx);
}

extern "C" void ADC_IRQHandler(void)
{
    HAL_ADC_IRQHandler(&hadc1);
}

// ─── PLLI2S configuration for exact 48 kHz ────────────────────────────────────

static void config_plli2s(void)
{
    // PLLI2S: N=192, R=5  (assumes PLLM=8 already set by SystemClock_Config)
    // VCO = 1 MHz × 192 = 192 MHz;  I2SCLK = 192 / 5 = 38.4 MHz
    RCC_PeriphCLKInitTypeDef pclk = {};
    pclk.PeriphClockSelection   = RCC_PERIPHCLK_I2S;
    pclk.PLLI2S.PLLI2SN         = 192;
    pclk.PLLI2S.PLLI2SR         = 5;
    HAL_RCCEx_PeriphCLKConfig(&pclk);
}

// ─── TIM3 — ADC trigger at 48 kHz ────────────────────────────────────────────

static void tim3_init(void)
{
    __HAL_RCC_TIM3_CLK_ENABLE();

    // TIM3 is on APB1; timer clock = 2 × APB1 = 2 × 42 MHz = 84 MHz
    // Period for 48 kHz: 84 000 000 / 48 000 − 1 = 1749
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
}

// ─── ADC1 — CH1 (PA1), triggered by TIM3 TRGO, DMA circular ─────────────────

static void adc_init(void)
{
    __HAL_RCC_ADC1_CLK_ENABLE();
    __HAL_RCC_DMA2_CLK_ENABLE();
    __HAL_RCC_GPIOA_CLK_ENABLE();

    // PA1 as analog input (no pull, no output)
    GPIO_InitTypeDef gin = {};
    gin.Pin  = GPIO_PIN_1;
    gin.Mode = GPIO_MODE_ANALOG;
    gin.Pull = GPIO_NOPULL;
    HAL_GPIO_Init(GPIOA, &gin);

    // DMA2 Stream0 CH0 for ADC1
    hdma_adc1.Instance                 = DMA2_Stream0;
    hdma_adc1.Init.Channel             = DMA_CHANNEL_0;
    hdma_adc1.Init.Direction           = DMA_PERIPH_TO_MEMORY;
    hdma_adc1.Init.PeriphInc           = DMA_PINC_DISABLE;
    hdma_adc1.Init.MemInc              = DMA_MINC_ENABLE;
    hdma_adc1.Init.PeriphDataAlignment = DMA_PDATAALIGN_HALFWORD;
    hdma_adc1.Init.MemDataAlignment    = DMA_MDATAALIGN_HALFWORD;
    hdma_adc1.Init.Mode                = DMA_CIRCULAR;
    hdma_adc1.Init.Priority            = DMA_PRIORITY_HIGH;
    hdma_adc1.Init.FIFOMode            = DMA_FIFOMODE_DISABLE;
    HAL_DMA_Init(&hdma_adc1);

    __HAL_LINKDMA(&hadc1, DMA_Handle, hdma_adc1);

    HAL_NVIC_SetPriority(DMA2_Stream0_IRQn, 2, 0);
    HAL_NVIC_EnableIRQ(DMA2_Stream0_IRQn);

    // ADC1
    hadc1.Instance                   = ADC1;
    hadc1.Init.ClockPrescaler        = ADC_CLOCK_SYNC_PCLK_DIV4;  // 84/4 = 21 MHz
    hadc1.Init.Resolution            = ADC_RESOLUTION_12B;
    hadc1.Init.ScanConvMode          = DISABLE;
    hadc1.Init.ContinuousConvMode    = DISABLE;
    hadc1.Init.DiscontinuousConvMode = DISABLE;
    hadc1.Init.ExternalTrigConvEdge  = ADC_EXTERNALTRIGCONVEDGE_RISING;
    hadc1.Init.ExternalTrigConv      = ADC_EXTERNALTRIGCONV_T3_TRGO;
    hadc1.Init.DataAlign             = ADC_DATAALIGN_RIGHT;
    hadc1.Init.NbrOfConversion       = 1;
    hadc1.Init.DMAContinuousRequests = ENABLE;
    hadc1.Init.EOCSelection          = ADC_EOC_SINGLE_CONV;
    HAL_ADC_Init(&hadc1);

    ADC_ChannelConfTypeDef ch = {};
    ch.Channel      = AUDIO_ADC_CHANNEL;
    ch.Rank         = 1;
    ch.SamplingTime = ADC_SAMPLETIME_15CYCLES;
    HAL_ADC_ConfigChannel(&hadc1, &ch);
}

// ─── I2S2 — Philips 16-bit stereo master TX, DMA circular ────────────────────

static void i2s_init(void)
{
    __HAL_RCC_SPI2_CLK_ENABLE();
    __HAL_RCC_DMA1_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();

    // PB12=WS, PB13=CK, PB15=SD  (AF5 = SPI2/I2S2)
    GPIO_InitTypeDef gin = {};
    gin.Pin       = GPIO_PIN_12 | GPIO_PIN_13 | GPIO_PIN_15;
    gin.Mode      = GPIO_MODE_AF_PP;
    gin.Pull      = GPIO_NOPULL;
    gin.Speed     = GPIO_SPEED_FREQ_HIGH;
    gin.Alternate = GPIO_AF5_SPI2;
    HAL_GPIO_Init(GPIOB, &gin);

    // DMA1 Stream4 CH0 for I2S2 TX
    hdma_i2s2_tx.Instance                 = DMA1_Stream4;
    hdma_i2s2_tx.Init.Channel             = DMA_CHANNEL_0;
    hdma_i2s2_tx.Init.Direction           = DMA_MEMORY_TO_PERIPH;
    hdma_i2s2_tx.Init.PeriphInc           = DMA_PINC_DISABLE;
    hdma_i2s2_tx.Init.MemInc              = DMA_MINC_ENABLE;
    hdma_i2s2_tx.Init.PeriphDataAlignment = DMA_PDATAALIGN_HALFWORD;
    hdma_i2s2_tx.Init.MemDataAlignment    = DMA_MDATAALIGN_HALFWORD;
    hdma_i2s2_tx.Init.Mode                = DMA_CIRCULAR;
    hdma_i2s2_tx.Init.Priority            = DMA_PRIORITY_HIGH;
    hdma_i2s2_tx.Init.FIFOMode            = DMA_FIFOMODE_DISABLE;
    HAL_DMA_Init(&hdma_i2s2_tx);

    __HAL_LINKDMA(&hi2s2, hdmatx, hdma_i2s2_tx);

    HAL_NVIC_SetPriority(DMA1_Stream4_IRQn, 2, 0);
    HAL_NVIC_EnableIRQ(DMA1_Stream4_IRQn);

    // I2S2: master TX, Philips std, 16-bit, 48 kHz
    // HAL will calculate prescaler from PLLI2S clock (38.4 MHz configured above).
    hi2s2.Instance         = SPI2;
    hi2s2.Init.Mode        = I2S_MODE_MASTER_TX;
    hi2s2.Init.Standard    = I2S_STANDARD_PHILIPS;
    hi2s2.Init.DataFormat  = I2S_DATAFORMAT_16B;
    hi2s2.Init.MCLKOutput  = I2S_MCLKOUTPUT_DISABLE;
    hi2s2.Init.AudioFreq   = I2S_AUDIOFREQ_48K;
    hi2s2.Init.CPOL        = I2S_CPOL_LOW;
    hi2s2.Init.ClockSource = I2S_CLOCK_PLL;
    hi2s2.Init.FullDuplexMode = I2S_FULLDUPLEXMODE_DISABLE;
    HAL_I2S_Init(&hi2s2);
}

// ─── Public API ───────────────────────────────────────────────────────────────

void audio_init(void)
{
    config_plli2s();
    tim3_init();
    adc_init();
    i2s_init();

    // Pre-fill output buffer with silence
    memset(i2s_buf, 0, sizeof(i2s_buf));
}

void audio_start(AudioCallback cb)
{
    s_callback = cb;

    // Start I2S DMA first (silence until ADC data arrives)
    HAL_I2S_Transmit_DMA(&hi2s2, (uint16_t*)i2s_buf,
                          AUDIO_BLOCK_FRAMES * 2 * 2);  // total halfwords

    // Start ADC DMA, then TIM3 trigger
    HAL_ADC_Start_DMA(&hadc1, (uint32_t*)adc_buf, AUDIO_BLOCK_FRAMES * 2);
    HAL_TIM_Base_Start(&htim3);
}

bool audio_overrun(void)
{
    bool v = s_overrun;
    s_overrun = false;
    return v;
}
