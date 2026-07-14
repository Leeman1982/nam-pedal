// audio.cpp — STM32F405 audio engine: ADC1 input + I2S2 PCM5102A output.
//
// Synchronisation model (I2S-driven double buffer):
//   The I2S2 DMA is the timing master.  Its TxHalf / TxCplt callbacks fire
//   once per block (1 ms each) and tell us exactly which half of the output
//   buffer just finished transmitting and is safe to overwrite.  We snapshot
//   the matching half of the ADC circular buffer, run the NAM callback, and
//   write the result back — all before the I2S circles back to that half.
//
//   ADC1   → DMA2 Stream0 CH0, circular, 16-bit, buffer = 2 × BLOCK (96 samples)
//            Runs free at 48 kHz (TIM3 trigger). No DMA callbacks needed.
//   I2S2TX → DMA1 Stream4 CH0, circular, 16-bit, buffer = 2 × BLOCK × 2ch = 192 hw
//            TxHalfCplt / TxCplt callbacks drive all audio processing.
//
// PCM5102A pin wiring: BCK→PB13, LRCK→PB12, DIN→PB15  (AF5 = SPI2/I2S2)
//   Strap FMT→GND (I2S), DEMP→GND, XSMT→3V3 (unmute), SCK→GND/float.
//
// PLLI2S for exact 48 kHz (HSE=8 MHz, PLLM=8 → VCO-in = 1 MHz):
//   N=192, R=5 → I2SCLK = 38.4 MHz
//   I2S prescaler: DIV=12, ODD=1 → BCK = 38.4/(2×12+1) = 1.536 MHz → fs=48000 Hz

#include "audio.h"
#include "config.h"
#include <Arduino.h>
#include <stm32f4xx_hal.h>
#include <string.h>

// ─── DMA buffers — must stay in AHB SRAM (NOT CCM: 0x10000000 is CPU-only) ───

// ADC circular buffer: 2 halves × BLOCK samples, filled continuously by DMA.
static uint16_t adc_buf[AUDIO_BLOCK_FRAMES * 2];

// I2S TX buffer: interleaved stereo 16-bit, 2 halves × BLOCK frames × 2 channels.
static int16_t  i2s_buf[AUDIO_BLOCK_FRAMES * 2 * 2];

// Float scratch (not DMA-mapped — CCM would be fine but not needed).
static float f_in [AUDIO_BLOCK_FRAMES];
static float f_out[AUDIO_BLOCK_FRAMES];

// ─── HAL handles ──────────────────────────────────────────────────────────────

static ADC_HandleTypeDef hadc1;
static DMA_HandleTypeDef hdma_adc1;
static I2S_HandleTypeDef hi2s2;
static DMA_HandleTypeDef hdma_i2s2_tx;
static TIM_HandleTypeDef htim3;

static AudioCallback s_callback = nullptr;
static volatile bool s_overrun  = false;
static volatile bool s_busy     = false;

// Alternates 0 → 1 → 0 on each I2S DMA callback so we always fill the half
// that just finished transmitting.
static int s_i2s_half = 0;

// ─── Core processing — runs in I2S DMA ISR context ───────────────────────────

static void process_i2s_half(void)
{
    if (s_busy) { s_overrun = true; return; }
    s_busy = true;

    // FPU: enable Flush-to-Zero and Default-NaN on Cortex-M4F
    __set_FPSCR(__get_FPSCR() | (1U << 24) | (1U << 25));

    int h = s_i2s_half;
    s_i2s_half ^= 1;

    // Snapshot the matching ADC half (DMA may still be writing the OTHER half).
    // Since both run at 48 kHz the snapshot is at most one sample stale — inaudible.
    const uint16_t* adc_half = adc_buf + h * AUDIO_BLOCK_FRAMES;
    int16_t*        i2s_half = i2s_buf + h * AUDIO_BLOCK_FRAMES * 2;

    // ADC 12-bit (0–4095, biased to 1.65 V mid-rail) → float –1 … +1
    for (size_t i = 0; i < AUDIO_BLOCK_FRAMES; i++)
        f_in[i] = ((float)adc_half[i] - 2048.0f) * (1.0f / 2048.0f);

    memset(f_out, 0, sizeof(f_out));

    if (s_callback)
        s_callback(f_in, f_out, AUDIO_BLOCK_FRAMES);

    // float → 16-bit signed stereo (mono NAM → both L and R channels)
    for (size_t i = 0; i < AUDIO_BLOCK_FRAMES; i++)
    {
        float s = f_out[i];
        if (s >  1.0f) s =  1.0f;
        if (s < -1.0f) s = -1.0f;
        int16_t pcm             = (int16_t)(s * 32767.0f);
        i2s_half[i * 2]     = pcm;   // L
        i2s_half[i * 2 + 1] = pcm;   // R
    }

    s_busy = false;
}

// ─── I2S DMA callbacks — override HAL weak symbols ───────────────────────────

// TxHalfCplt: I2S just finished transmitting half 0. Safe to write half 0 now.
extern "C" void HAL_I2S_TxHalfCpltCallback(I2S_HandleTypeDef* h)
{
    (void)h;
    process_i2s_half();
}

// TxCplt: I2S just finished transmitting half 1. Safe to write half 1 now.
extern "C" void HAL_I2S_TxCpltCallback(I2S_HandleTypeDef* h)
{
    (void)h;
    process_i2s_half();
}

// ─── DMA / peripheral IRQ handlers ───────────────────────────────────────────

extern "C" void DMA1_Stream4_IRQHandler(void)   // I2S2 TX
{
    HAL_DMA_IRQHandler(&hdma_i2s2_tx);
}

extern "C" void DMA2_Stream0_IRQHandler(void)   // ADC1
{
    HAL_DMA_IRQHandler(&hdma_adc1);
}

extern "C" void SPI2_IRQHandler(void)           // I2S2 peripheral errors
{
    HAL_I2S_IRQHandler(&hi2s2);
}

// ─── PLLI2S — 38.4 MHz I2S clock → exact 48 kHz sample rate ─────────────────

static void config_plli2s(void)
{
    RCC_PeriphCLKInitTypeDef pclk = {};
    pclk.PeriphClockSelection = RCC_PERIPHCLK_I2S;
    pclk.PLLI2S.PLLI2SN       = 192;  // VCO = 1 MHz × 192 = 192 MHz
    pclk.PLLI2S.PLLI2SR       = 5;    // I2SCLK = 192 / 5 = 38.4 MHz
    HAL_RCCEx_PeriphCLKConfig(&pclk);
}

// ─── TIM3 — 48 kHz ADC trigger ───────────────────────────────────────────────

static void tim3_init(void)
{
    __HAL_RCC_TIM3_CLK_ENABLE();

    // APB1 timer clock = 2 × 42 MHz = 84 MHz → period = 84 000 000 / 48 000 − 1 = 1749
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

// ─── ADC1 — PA1 (CH1), TIM3-triggered, DMA2 Stream0 circular ─────────────────

static void adc_init(void)
{
    __HAL_RCC_ADC1_CLK_ENABLE();
    __HAL_RCC_DMA2_CLK_ENABLE();
    __HAL_RCC_GPIOA_CLK_ENABLE();

    GPIO_InitTypeDef gin = {};
    gin.Pin  = GPIO_PIN_1;
    gin.Mode = GPIO_MODE_ANALOG;
    gin.Pull = GPIO_NOPULL;
    HAL_GPIO_Init(GPIOA, &gin);

    // DMA2 Stream0 CH0 — circular, no IRQ needed (I2S callbacks drive processing)
    hdma_adc1.Instance                 = DMA2_Stream0;
    hdma_adc1.Init.Channel             = DMA_CHANNEL_0;
    hdma_adc1.Init.Direction           = DMA_PERIPH_TO_MEMORY;
    hdma_adc1.Init.PeriphInc           = DMA_PINC_DISABLE;
    hdma_adc1.Init.MemInc              = DMA_MINC_ENABLE;
    hdma_adc1.Init.PeriphDataAlignment = DMA_PDATAALIGN_HALFWORD;
    hdma_adc1.Init.MemDataAlignment    = DMA_MDATAALIGN_HALFWORD;
    hdma_adc1.Init.Mode                = DMA_CIRCULAR;
    hdma_adc1.Init.Priority            = DMA_PRIORITY_MEDIUM;
    hdma_adc1.Init.FIFOMode            = DMA_FIFOMODE_DISABLE;
    HAL_DMA_Init(&hdma_adc1);
    __HAL_LINKDMA(&hadc1, DMA_Handle, hdma_adc1);

    // ADC DMA IRQ at low priority — only handles errors, not audio timing
    HAL_NVIC_SetPriority(DMA2_Stream0_IRQn, 5, 0);
    HAL_NVIC_EnableIRQ(DMA2_Stream0_IRQn);

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

// ─── I2S2 — PCM5102A, Philips 16-bit stereo master TX, DMA1 Stream4 ──────────

static void i2s_init(void)
{
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

    // DMA1 Stream4 CH0 — I2S2 TX, circular, high priority (drives audio timing)
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

    // I2S2 TX DMA interrupt — highest audio priority
    HAL_NVIC_SetPriority(DMA1_Stream4_IRQn, 1, 0);
    HAL_NVIC_EnableIRQ(DMA1_Stream4_IRQn);

    // I2S2 peripheral interrupt (error handling)
    HAL_NVIC_SetPriority(SPI2_IRQn, 1, 1);
    HAL_NVIC_EnableIRQ(SPI2_IRQn);

    // I2S2: master TX, Philips standard, 16-bit data, 48 kHz.
    // HAL calculates the I2S prescaler (DIV=12, ODD=1) from the PLLI2S clock.
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
}

// ─── Public API ───────────────────────────────────────────────────────────────

void audio_init(void)
{
    config_plli2s();
    tim3_init();
    adc_init();
    i2s_init();
    memset(i2s_buf, 0, sizeof(i2s_buf));
}

void audio_start(AudioCallback cb)
{
    s_callback = cb;

    // Start I2S DMA first — this becomes the timing master.
    // Total buffer is BLOCK*2*2 = 192 halfwords.
    HAL_I2S_Transmit_DMA(&hi2s2, (uint16_t*)i2s_buf,
                          AUDIO_BLOCK_FRAMES * 2 * 2);

    // ADC starts free-running; TIM3 paces it at 48 kHz.
    HAL_ADC_Start_DMA(&hadc1, (uint32_t*)adc_buf, AUDIO_BLOCK_FRAMES * 2);
    HAL_TIM_Base_Start(&htim3);
}

bool audio_overrun(void)
{
    bool v = s_overrun;
    s_overrun = false;
    return v;
}
