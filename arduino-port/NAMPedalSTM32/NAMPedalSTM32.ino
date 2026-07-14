// NAMPedalSTM32 — Neural Amp Modeler on WeAct STM32F405RGT6
//
// Audio pipeline:
//   Guitar → ADC1/PA1 (TIM3-triggered DMA, 48 kHz) → NAM inference
//          → I2S2 DMA double-buffer (audio.cpp) → PCM5102A DAC → amp/headphones
//
// Libraries required (install via Arduino Library Manager):
//   • STM32duino core  (Board Manager URL in INSTALL.md)
//   • "U8g2"           by Oliver Kraus
//   • "SD"             (built-in Arduino)
//
// NAM source files: see INSTALL.md for how to install NAMCore library.

#include <Arduino.h>
#include <Wire.h>

#include "config.h"
#include "audio.h"
#include "model_mgr.h"
#include "ui.h"

// NAM inference engine (installed as NAMCore Arduino library — see INSTALL.md)
#include <NAMCore.h>
#include <memory>

// ─── Globals ──────────────────────────────────────────────────────────────────

static std::unique_ptr<nam::DSP> g_model;

// Protect model swap from being interrupted mid-swap.
// We briefly set g_effect_active=false while the unique_ptr is being replaced;
// the ISR-safe pattern is: false → swap → DMB → true.
static volatile bool  g_effect_active = false;
static volatile float g_gain          = 0.5f;
static volatile float g_vol           = 1.0f;

static uint8_t g_model_buf[MODEL_MAX_FILE_BYTES];

static ModelEntry g_model_list[MODEL_MAX_COUNT];
static uint8_t    g_model_count = 0;
static char       g_current_model[64] = "none";

// Per-block float scratch (plain globals — accessible from loop(), not ISR)
static float f_in [AUDIO_BLOCK_FRAMES];
static float f_out[AUDIO_BLOCK_FRAMES];
static NAM_SAMPLE nam_in [AUDIO_BLOCK_FRAMES];
static NAM_SAMPLE nam_out[AUDIO_BLOCK_FRAMES];

// I2S stereo output buffer (48 frames × 2 channels × 2 bytes)
static int16_t i2s_write_buf[AUDIO_BLOCK_FRAMES * 2];

// ─── LED helper ───────────────────────────────────────────────────────────────

static void led_set(bool on)
{
    bool level = STATUS_LED_INVERT ? !on : on;
    digitalWrite(STATUS_LED_PIN, level ? HIGH : LOW);
}

// ─── Model loading ────────────────────────────────────────────────────────────

static bool load_model(const char* filename)
{
    char path[68];
    if (filename[0] != '/')
        snprintf(path, sizeof(path), "/%s", filename);
    else
        strncpy(path, filename, sizeof(path) - 1);

    size_t bytes = model_load_file(path, g_model_buf, sizeof(g_model_buf));
    if (bytes == 0)
    {
        Serial.print("Load failed: "); Serial.println(path);
        return false;
    }
    Serial.print("Read "); Serial.print((unsigned)bytes); Serial.println(" bytes");

    uint32_t t0 = millis();
    std::unique_ptr<nam::DSP> tmp;
    try {
        tmp = nam::get_dsp_namb(g_model_buf, bytes);
    }
    catch (const std::exception& e) {
        Serial.print("NAM exception: "); Serial.println(e.what());
        return false;
    }

    if (!tmp) { Serial.println("get_dsp_namb returned null"); return false; }

    tmp->ResetAndPrewarm((double)AUDIO_SAMPLE_RATE, AUDIO_BLOCK_FRAMES);

    // Pause effect → swap model → memory barrier → resume.
    // The audio runs entirely in loop() context (no ISR), so this is safe.
    g_effect_active = false;
    __DMB();                          // data memory barrier — no reordering
    g_model = std::move(tmp);
    __DMB();
    g_effect_active = true;

    strncpy(g_current_model, filename, sizeof(g_current_model) - 1);
    g_current_model[sizeof(g_current_model) - 1] = '\0';

    Serial.print("Model ready in "); Serial.print(millis() - t0); Serial.println(" ms");
    return true;
}

// ─── Setup ────────────────────────────────────────────────────────────────────

void setup(void)
{
    Serial.begin(115200);

    // FPU: enable Flush-to-Zero + Default-NaN for fast-math on Cortex-M4F
    uint32_t fpscr = __get_FPSCR();
    fpscr |= (1U << 24) | (1U << 25);
    __set_FPSCR(fpscr);
    // Apply to all future FPU contexts
    *reinterpret_cast<volatile uint32_t*>(0xE000EF3C) |= (1U << 24) | (1U << 25);

    pinMode(STATUS_LED_PIN, OUTPUT);
    led_set(false);

    // I2C for OLED
    Wire.begin();
    ui_init();
    delay(1200);

    // NAM fast tanh — avoids exp() in inference hot path
    nam::activations::Activation::enable_fast_tanh();

    // Scan SD
    Serial.println("Scanning SD...");
    g_model_count = model_scan(g_model_list, MODEL_MAX_COUNT);
    Serial.print(g_model_count); Serial.println(" model(s) found");
    ui_set_model_list(g_model_list, g_model_count);

    // Load last-used or first available model
    char saved[64] = {};
    const char* to_load = nullptr;
    if (model_load_config(saved, sizeof(saved)))
        for (uint8_t i = 0; i < g_model_count; i++)
            if (strcmp(g_model_list[i].name, saved) == 0)
                { to_load = g_model_list[i].name; break; }

    if (!to_load && g_model_count > 0)
        to_load = g_model_list[0].name;

    if (to_load)
    {
        if (load_model(to_load))
            model_save_config(to_load);
    }
    else
    {
        Serial.println("No models — bypass mode");
        strncpy(g_current_model, "no model", sizeof(g_current_model));
    }

    ui_set_status(g_current_model, (g_model != nullptr), g_gain, g_vol);

    // Start ADC driver
    audio_adc_init();
    audio_adc_start();

    // Start I2S output (PLLI2S + I2S2 + circular DMA — see audio.cpp)
    audio_i2s_init();
    audio_i2s_start();

    g_effect_active = (g_model != nullptr);
    led_set(g_effect_active);
    ui_set_status(g_current_model, g_effect_active, g_gain, g_vol);

    Serial.println("Audio started");
}

// ─── Main loop — audio + UI ───────────────────────────────────────────────────

// UI is updated every kUiPeriodMs to avoid starving the audio loop.
static constexpr uint32_t kUiPeriodMs = 30;  // ~33 fps
static uint32_t last_ui_ms  = 0;
static uint32_t last_log_ms = 0;

void loop(void)
{
    // ── 1. Collect 48 ADC samples (blocks up to 3 ms) ────────────────────────
    bool got_audio = audio_adc_read(f_in, AUDIO_BLOCK_FRAMES, 3);

    // ── 2. Process through NAM ────────────────────────────────────────────────
    if (got_audio)
    {
        if (g_model && g_effect_active)
        {
            float gain = g_gain;
            float vol  = g_vol;
            for (size_t i = 0; i < AUDIO_BLOCK_FRAMES; i++)
                nam_in[i] = f_in[i] * gain;

            NAM_SAMPLE* ip = nam_in;
            NAM_SAMPLE* op = nam_out;
            g_model->process(&ip, &op, AUDIO_BLOCK_FRAMES);

            for (size_t i = 0; i < AUDIO_BLOCK_FRAMES; i++)
                f_out[i] = nam_out[i] * vol;
        }
        else
        {
            float vol = g_vol;
            for (size_t i = 0; i < AUDIO_BLOCK_FRAMES; i++)
                f_out[i] = f_in[i] * vol;
        }

        // Clamp + convert to 16-bit stereo for PCM5102A
        for (size_t i = 0; i < AUDIO_BLOCK_FRAMES; i++)
        {
            float s = f_out[i];
            if (s >  1.0f) s =  1.0f;
            if (s < -1.0f) s = -1.0f;
            int16_t pcm            = (int16_t)(s * 32767.0f);
            i2s_write_buf[i * 2]     = pcm;  // L
            i2s_write_buf[i * 2 + 1] = pcm;  // R
        }

        // Write to PCM5102A (blocks until a DMA half-buffer is free, ≤1 ms)
        audio_i2s_write(i2s_write_buf, AUDIO_BLOCK_FRAMES);
    }

    // ── 3. UI (throttled to kUiPeriodMs) ─────────────────────────────────────
    uint32_t now = millis();
    if (now - last_ui_ms >= kUiPeriodMs)
    {
        last_ui_ms = now;

        UiEvent ev = ui_poll();
        switch (ev)
        {
        case UI_LOAD_MODEL:
        {
            uint8_t idx = ui_selected_index();
            if (idx < g_model_count)
            {
                Serial.print("Loading: "); Serial.println(g_model_list[idx].name);
                if (load_model(g_model_list[idx].name))
                    model_save_config(g_model_list[idx].name);
                led_set(g_effect_active);
                ui_set_status(g_current_model, g_effect_active, g_gain, g_vol);
            }
            break;
        }
        case UI_TOGGLE_BYPASS:
            g_effect_active = !g_effect_active;
            led_set(g_effect_active);
            ui_set_status(g_current_model, g_effect_active, g_gain, g_vol);
            break;

        case UI_GAIN_CHANGED:
            g_gain = ui_gain();
            ui_set_status(g_current_model, g_effect_active, g_gain, g_vol);
            break;

        case UI_VOL_CHANGED:
            g_vol = ui_vol();
            ui_set_status(g_current_model, g_effect_active, g_gain, g_vol);
            break;

        default:
            break;
        }

        // Serial heartbeat once per second
        if (now - last_log_ms >= 1000)
        {
            last_log_ms = now;
            Serial.printf("model=%s  gain=%.2f  vol=%.2f  %s\r\n",
                g_current_model, (double)g_gain, (double)g_vol,
                g_effect_active ? "ACTIVE" : "BYPASS");
        }
    }
}
