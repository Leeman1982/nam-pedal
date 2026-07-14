#pragma once

// ─── Audio ────────────────────────────────────────────────────────────────────
#define AUDIO_SAMPLE_RATE   48000U
#define AUDIO_BLOCK_FRAMES  48U

// ADC guitar input: PA1 (ADC1_IN1).
// Wire: guitar → 100k/100k divider to 1.65 V bias, 100 nF DC-block cap.
#define AUDIO_ADC_CHANNEL   ADC_CHANNEL_1   // PA1

// PCM5102A I2S pins (I2S2 / SPI2, AF5).
// Strap: FMT→GND, DEMP→GND, XSMT→3V3, SCK→GND/float.
#define I2S_PIN_BCK   PB13   // BCK
#define I2S_PIN_WS    PB12   // LRCK
#define I2S_PIN_DATA  PB15   // DIN

// ─── SD card (SPI1) ──────────────────────────────────────────────────────────
// Onboard SD on WeAct F405 board, or external SPI SD module.
#define SD_CS_PIN   PA4
#define SD_SCK_PIN  PA5
#define SD_MISO_PIN PA6
#define SD_MOSI_PIN PA7

#define MODEL_MAX_FILE_BYTES  (20U * 1024U)   // 20 KB — covers nano + feather
#define MODEL_MAX_COUNT       32U
#define MODEL_CONFIG_FILE     "/nam_cfg.txt"

// ─── OLED / I2C ───────────────────────────────────────────────────────────────
// SH1106 1.3" 128×64, I2C 0x3C, on I2C1 (SDA→PB9, SCL→PB8).
#define OLED_I2C_ADDR  0x3C

// ─── Rotary encoder ───────────────────────────────────────────────────────────
// EstarDyn module: CLK, DT, SW (active-low).
#define ENC_CLK_PIN        PC6
#define ENC_DT_PIN         PC7
#define ENC_SW_PIN         PC8
#define ENC_LONG_PRESS_MS  600U    // long-press → open model list
#define ENC_DBLCLICK_MS    400U    // double-click → bypass toggle

// ─── Status LED ──────────────────────────────────────────────────────────────
// WeAct F405 user LED on PC13 (active-low on most variants).
#define STATUS_LED_PIN    PC13
#define STATUS_LED_INVERT true
