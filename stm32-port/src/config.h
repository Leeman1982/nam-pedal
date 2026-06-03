#pragma once

// ─── Audio ────────────────────────────────────────────────────────────────────
// Sample rate and block size.  48 kHz / 48-sample blocks = 1 ms per block.
#define AUDIO_SAMPLE_RATE   48000U
#define AUDIO_BLOCK_FRAMES  48U

// ADC guitar input — PA1 (ADC1_IN1).
// Wire: guitar → 100 kΩ/100 kΩ voltage divider to 1.65 V bias, 100 nF DC-block cap.
#define AUDIO_ADC_PIN       PA1
#define AUDIO_ADC_INSTANCE  ADC1
#define AUDIO_ADC_CHANNEL   ADC_CHANNEL_1

// I2S2 output to PCM5102A DAC.
//   PB12 = I2S2_WS  (LRCK)
//   PB13 = I2S2_CK  (BCK)
//   PB15 = I2S2_SD  (DIN on PCM5102A)
// PCM5102A wiring: FMT→GND (I2S), DEMP→GND, XSMT→3V3 (unmute), SCK→GND/float.
#define AUDIO_I2S_INSTANCE  SPI2   // SPI2 operates as I2S2

// ─── SD card (SPI1) ──────────────────────────────────────────────────────────
// Onboard SD slot on WeAct F405 board uses SPI1.
// Adjust CS pin if your board differs — other SPI1 pins are fixed to PA5/PA6/PA7.
#define SD_CS_PIN   PA4
#define SD_SCK_PIN  PA5
#define SD_MISO_PIN PA6
#define SD_MOSI_PIN PA7

// Maximum .namb file size loaded into RAM (bytes).
// 16 KB covers nano and most feather models.
#define MODEL_MAX_FILE_BYTES  (16U * 1024U)

// Maximum number of models that can be listed from SD card.
#define MODEL_MAX_COUNT  32U

// Filename written to SD card to remember the last loaded model.
#define MODEL_CONFIG_FILE  "/nam_config.txt"

// ─── OLED / I2C ───────────────────────────────────────────────────────────────
// SH1106 1.3" 128×64, I2C address 0x3C, connected via I2C1.
// SDA→PB9, SCL→PB8  (Wire default for STM32duino on F4 boards)
#define OLED_I2C_ADDR  0x3C

// ─── Rotary encoder ───────────────────────────────────────────────────────────
// EstarDyn module: CLK, DT, SW (active-low button with internal pull-up).
#define ENC_CLK_PIN  PC6
#define ENC_DT_PIN   PC7
#define ENC_SW_PIN   PC8

// Long-press threshold in milliseconds (enter / leave model-select screen).
#define ENC_LONG_PRESS_MS  600U

// ─── Status LED ──────────────────────────────────────────────────────────────
// WeAct F405 has a user LED on PC13 (active-low on most variants).
#define STATUS_LED_PIN    PC13
#define STATUS_LED_INVERT true   // set false if your board is active-high
