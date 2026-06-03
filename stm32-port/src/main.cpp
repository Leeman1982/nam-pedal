// main.cpp — NAMPedal STM32F405 port
//
// Hardware: WeAct STM32F405RGT6 core board (168 MHz Cortex-M4F)
//   Audio in:  Guitar → voltage divider + bias → PA1 (ADC1_IN1)
//   Audio out: I2S2 → PCM5102A DAC (PB12=LRCK, PB13=BCK, PB15=DIN)
//   Storage:   SPI1 SD card (PA4=CS, PA5=SCK, PA6=MISO, PA7=MOSI)
//   UI:        SH1106 1.3" OLED + EC11 rotary encoder on I2C1 (PB8/PB9)
//              Encoder: CLK=PC6, DT=PC7, SW=PC8
//
// Model format: .namb binary (nano / feather NAM A2 captures)
// Up to MODEL_MAX_COUNT models stored on SD; selection persists across reboots.

#include <Arduino.h>
#include <Wire.h>
#include "config.h"
#include "audio.h"
#include "model_mgr.h"
#include "ui.h"

// NAM core
#include "NAM/dsp.h"
#include "NAM/activations.h"
#include "namb/get_dsp_namb.h"
#include <memory>

// ─── State ────────────────────────────────────────────────────────────────────

static std::unique_ptr<nam::DSP> g_model;
static volatile bool g_effect_active = false;
static volatile float g_gain   = 0.5f;
static volatile float g_vol    = 1.0f;

// File buffer lives in SRAM (NOT CCM — must be DMA-accessible for potential
// future DMA-based SD reads).
static uint8_t g_model_buf[MODEL_MAX_FILE_BYTES];

static ModelEntry g_model_list[MODEL_MAX_COUNT];
static uint8_t    g_model_count = 0;
static char       g_current_model[64] = "none";

// Status LED helpers
static inline void led_set(bool on)
{
    bool level = STATUS_LED_INVERT ? !on : on;
    digitalWrite(STATUS_LED_PIN, level ? HIGH : LOW);
}

// ─── Audio callback (called from DMA ISR, ~1 ms deadline) ────────────────────

static void audio_cb(const float* in, float* out, size_t frames)
{
    if (g_model && g_effect_active)
    {
        // Apply input gain into scratch buffer
        static NAM_SAMPLE in_gain[AUDIO_BLOCK_FRAMES];
        static NAM_SAMPLE out_nam[AUDIO_BLOCK_FRAMES];
        float gain = g_gain;
        for (size_t i = 0; i < frames; i++)
            in_gain[i] = in[i] * gain;

        // process() advances the pointers; keep local copies
        NAM_SAMPLE* ip = in_gain;
        NAM_SAMPLE* op = out_nam;
        g_model->process(&ip, &op, frames);

        // Apply output volume and copy to caller's buffer
        float vol = g_vol;
        for (size_t i = 0; i < frames; i++)
            out[i] = out_nam[i] * vol;
    }
    else
    {
        // Bypass: clean passthrough with output volume
        float vol = g_vol;
        for (size_t i = 0; i < frames; i++)
            out[i] = in[i] * vol;
    }
}

// ─── Model loading ────────────────────────────────────────────────────────────

static bool load_model(const char* filename)
{
    // Build full path
    char path[68];
    if (filename[0] != '/')
        snprintf(path, sizeof(path), "/%s", filename);
    else
        strncpy(path, filename, sizeof(path) - 1);

    size_t bytes = model_load_file(path, g_model_buf, sizeof(g_model_buf));
    if (bytes == 0)
    {
        Serial.print("model_load_file failed: ");
        Serial.println(path);
        return false;
    }
    Serial.print("Read "); Serial.print(bytes); Serial.println(" bytes");

    uint32_t t0 = millis();
    std::unique_ptr<nam::DSP> tmp;
    try {
        tmp = nam::get_dsp_namb(g_model_buf, bytes);
    } catch (const std::exception& e) {
        Serial.print("NAM exception: "); Serial.println(e.what());
        return false;
    }

    if (!tmp)
    {
        Serial.println("get_dsp_namb returned null");
        return false;
    }

    tmp->ResetAndPrewarm((double)AUDIO_SAMPLE_RATE, AUDIO_BLOCK_FRAMES);

    // Swap atomically-ish: pause effect, swap, resume
    bool was_active = g_effect_active;
    g_effect_active = false;
    g_model = std::move(tmp);
    g_effect_active = was_active;

    strncpy(g_current_model, filename, sizeof(g_current_model) - 1);
    g_current_model[sizeof(g_current_model) - 1] = '\0';

    Serial.print("Model loaded in "); Serial.print(millis() - t0); Serial.println(" ms");
    return true;
}

// ─── Setup ────────────────────────────────────────────────────────────────────

void setup(void)
{
    Serial.begin(115200);

    // Enable FPU Flush-to-Zero + Default-NaN (Cortex-M4F)
    uint32_t fpscr = __get_FPSCR();
    fpscr |= (1U << 24) | (1U << 25);
    __set_FPSCR(fpscr);
    // Apply same defaults to all future FPU exception contexts
    volatile uint32_t* FPDSCR = reinterpret_cast<volatile uint32_t*>(0xE000EF3C);
    *FPDSCR |= (1U << 24) | (1U << 25);

    // Status LED
    pinMode(STATUS_LED_PIN, OUTPUT);
    led_set(false);

    // I2C for OLED (PB8=SCL, PB9=SDA is the Wire default for STM32duino F4)
    Wire.begin();

    // Init UI — shows splash screen
    ui_init();
    delay(1000);

    // Enable fast tanh (avoids calling exp() in hot path)
    nam::activations::Activation::enable_fast_tanh();

    // Scan SD card for models
    Serial.println("Scanning SD...");
    g_model_count = model_scan(g_model_list, MODEL_MAX_COUNT);
    Serial.print(g_model_count); Serial.println(" models found");
    ui_set_model_list(g_model_list, g_model_count);

    // Try to load the last-used model
    char saved[64] = {};
    bool have_saved = model_load_config(saved, sizeof(saved));
    const char* first_to_load = nullptr;

    if (have_saved)
    {
        // Verify the saved filename still exists on the card
        for (uint8_t i = 0; i < g_model_count; i++)
        {
            if (strcmp(g_model_list[i].name, saved) == 0)
            {
                first_to_load = g_model_list[i].name;
                break;
            }
        }
    }
    if (!first_to_load && g_model_count > 0)
        first_to_load = g_model_list[0].name;

    if (first_to_load)
    {
        Serial.print("Loading: "); Serial.println(first_to_load);
        if (load_model(first_to_load))
            model_save_config(first_to_load);
    }
    else
    {
        Serial.println("No models on SD — running bypass");
        strncpy(g_current_model, "no model", sizeof(g_current_model));
    }

    ui_set_status(g_current_model, g_effect_active, g_gain, g_vol);

    // Enable DWT cycle counter for optional profiling
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0;
    DWT->CTRL  |= DWT_CTRL_CYCCNTENA_Msk;

    // Start audio engine
    audio_init();
    audio_start(audio_cb);

    g_effect_active = (g_model != nullptr);
    led_set(g_effect_active);

    ui_set_status(g_current_model, g_effect_active, g_gain, g_vol);
    Serial.println("Audio engine started");
}

// ─── Main loop ────────────────────────────────────────────────────────────────

static uint32_t last_ui_redraw  = 0;
static uint32_t last_status_log = 0;

void loop(void)
{
    UiEvent ev = ui_poll();

    switch (ev)
    {
    case UI_LOAD_MODEL:
    {
        uint8_t idx = ui_selected_index();
        if (idx < g_model_count)
        {
            const char* name = g_model_list[idx].name;
            Serial.print("Loading model: "); Serial.println(name);
            if (load_model(name))
            {
                model_save_config(name);
                g_effect_active = true;
            }
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

    // Periodic OLED refresh even when nothing changes (~10 fps)
    uint32_t now = millis();
    if (now - last_ui_redraw >= 100)
    {
        last_ui_redraw = now;
        ui_set_status(g_current_model, g_effect_active, g_gain, g_vol);
    }

    // Serial diagnostics once per second
    if (now - last_status_log >= 1000)
    {
        last_status_log = now;
        Serial.printf("model=%s  gain=%.2f  vol=%.2f  %s%s\n",
            g_current_model, (double)g_gain, (double)g_vol,
            g_effect_active ? "ACTIVE" : "BYPASS",
            audio_overrun() ? "  *** OVERRUN ***" : "");
    }
}
