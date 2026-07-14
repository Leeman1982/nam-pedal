// ui.cpp — OLED (SH1106, U8g2) + EC11 rotary encoder UI.
//
// Screens:
//   HOME         — model name, gain bar, volume bar, bypass state.
//                  Rotate: adjusts the currently selected parameter (gain/vol).
//                  Short press: toggle between adjusting gain and volume.
//                  Long press: open model-select screen.
//   MODEL_SELECT — scrollable list of .namb files.
//                  Rotate: scroll.
//                  Short press: load selected model → UI_LOAD_MODEL event.
//                  Long press: back to home.

#include "ui.h"
#include "config.h"
#include <Arduino.h>
#include <Wire.h>
#include <U8g2lib.h>
#include <string.h>

// ─── OLED ─────────────────────────────────────────────────────────────────────

// SH1106 128×64 I2C full-frame-buffer, hardware I2C.
// Pins: SDA→PB9, SCL→PB8 (Wire default for STM32duino F4).
static U8G2_SH1106_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);

// ─── Encoder state ────────────────────────────────────────────────────────────

static volatile int8_t  enc_delta   = 0;   // accumulated rotation (+/-)
static volatile bool    enc_pressed = false;
static uint32_t         enc_press_start = 0;
static bool             enc_was_down    = false;

static void enc_clk_isr(void)
{
    // Quadrature decode: when CLK falls, sample DT.
    bool dt = digitalRead(ENC_DT_PIN);
    enc_delta += dt ? +1 : -1;
}

// ─── Model list ───────────────────────────────────────────────────────────────

static const ModelEntry* s_list       = nullptr;
static uint8_t           s_list_count = 0;

// ─── Home screen state ────────────────────────────────────────────────────────

static char  s_model_name[64]  = "none";
static bool  s_active          = false;
static float s_gain            = 0.5f;
static float s_vol             = 1.0f;

// ─── App state ────────────────────────────────────────────────────────────────

enum Screen { SCREEN_HOME, SCREEN_MODEL_SELECT };
static Screen  s_screen     = SCREEN_HOME;
static uint8_t s_sel_idx    = 0;   // selected index in model-select
static uint8_t s_scroll_top = 0;   // first visible row in model-select
static bool    s_edit_vol   = false; // false=adjust gain, true=adjust vol

static UiEvent s_pending_event  = UI_NONE;
static uint8_t s_load_index     = 0;

// Step size per encoder click for gain/vol
static constexpr float kStep = 0.025f;

// Visible rows in model-select screen (128×64, status bar 10px, rows 14px)
static constexpr uint8_t kVisibleRows = 3;

// ─── Draw helpers ─────────────────────────────────────────────────────────────

// Draw a horizontal progress bar. Returns pixel width of filled portion.
static void draw_bar(uint8_t x, uint8_t y, uint8_t w, uint8_t h, float v)
{
    u8g2.drawFrame(x, y, w, h);
    uint8_t fill = (uint8_t)(v * (float)(w - 2));
    if (fill > w - 2) fill = w - 2;
    if (fill > 0)
        u8g2.drawBox(x + 1, y + 1, fill, h - 2);
}

// ─── Screen renderers ─────────────────────────────────────────────────────────

static void draw_home(void)
{
    u8g2.clearBuffer();

    // Status bar
    u8g2.setFont(u8g2_font_5x7_tr);
    u8g2.drawStr(0, 7, "NAM Pedal");
    const char* status_str = s_active ? "[ON]" : "[BYPASS]";
    u8g2.drawStr(128 - u8g2.getStrWidth(status_str), 7, status_str);
    u8g2.drawHLine(0, 9, 128);

    // Model name (truncate to 20 chars to fit screen)
    u8g2.setFont(u8g2_font_5x7_tr);
    char trunc[22];
    strncpy(trunc, s_model_name, 21);
    trunc[21] = '\0';
    u8g2.drawStr(0, 20, trunc);

    // Gain bar
    bool gain_sel = !s_edit_vol;
    u8g2.setFont(u8g2_font_5x7_tr);
    if (gain_sel) u8g2.setDrawColor(1);
    u8g2.drawStr(0, 33, gain_sel ? ">GAIN" : " GAIN");
    draw_bar(36, 26, 72, 8, s_gain);
    char val_str[8];
    snprintf(val_str, sizeof(val_str), "%.2f", s_gain);
    u8g2.drawStr(112, 33, val_str);  // right-aligned area

    // Volume bar
    bool vol_sel = s_edit_vol;
    u8g2.drawStr(0, 47, vol_sel ? ">VOL " : " VOL ");
    draw_bar(36, 40, 72, 8, s_vol);
    snprintf(val_str, sizeof(val_str), "%.2f", s_vol);
    u8g2.drawStr(112, 47, val_str);

    // Hint line
    u8g2.setFont(u8g2_font_4x6_tr);
    u8g2.drawStr(0, 63, "PRESS:param  HOLD:models");

    u8g2.sendBuffer();
}

static void draw_model_select(void)
{
    u8g2.clearBuffer();

    u8g2.setFont(u8g2_font_5x7_tr);
    u8g2.drawStr(0, 7, "Select Model");
    u8g2.drawHLine(0, 9, 128);

    if (s_list_count == 0)
    {
        u8g2.drawStr(0, 30, "No .namb on SD");
    }
    else
    {
        for (uint8_t r = 0; r < kVisibleRows; r++)
        {
            uint8_t idx = s_scroll_top + r;
            if (idx >= s_list_count) break;

            uint8_t y = 21 + r * 14;
            bool selected = (idx == s_sel_idx);
            if (selected)
            {
                u8g2.drawBox(0, y - 10, 128, 12);
                u8g2.setDrawColor(0);
            }
            // Truncate to 24 chars
            char trunc[26];
            strncpy(trunc, s_list[idx].name, 25);
            trunc[25] = '\0';
            u8g2.drawStr(4, y, trunc);
            if (selected) u8g2.setDrawColor(1);
        }
    }

    // Footer: count + hint
    u8g2.setFont(u8g2_font_4x6_tr);
    char footer[32];
    snprintf(footer, sizeof(footer), "%u/%u  PRESS:load  HOLD:back",
             (unsigned)(s_sel_idx + 1), (unsigned)s_list_count);
    u8g2.drawStr(0, 63, footer);

    u8g2.sendBuffer();
}

static void redraw(void)
{
    if (s_screen == SCREEN_HOME)
        draw_home();
    else
        draw_model_select();
}

// ─── Public API ───────────────────────────────────────────────────────────────

void ui_init(void)
{
    // I2C clock speed
    Wire.setClock(400000);
    u8g2.begin();

    // Encoder pins with pull-up
    pinMode(ENC_CLK_PIN, INPUT_PULLUP);
    pinMode(ENC_DT_PIN,  INPUT_PULLUP);
    pinMode(ENC_SW_PIN,  INPUT_PULLUP);

    // Interrupt on CLK falling edge
    attachInterrupt(digitalPinToInterrupt(ENC_CLK_PIN), enc_clk_isr, FALLING);

    // Splash
    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_7x14B_tr);
    u8g2.drawStr(10, 28, "NAM Pedal");
    u8g2.setFont(u8g2_font_5x7_tr);
    u8g2.drawStr(10, 46, "STM32F405  v1.0");
    u8g2.sendBuffer();
}

void ui_set_model_list(const ModelEntry* list, uint8_t count)
{
    s_list       = list;
    s_list_count = count;
    s_sel_idx    = 0;
    s_scroll_top = 0;
}

void ui_set_status(const char* model_name, bool active, float gain, float vol)
{
    strncpy(s_model_name, model_name, sizeof(s_model_name) - 1);
    s_model_name[sizeof(s_model_name) - 1] = '\0';
    s_active = active;
    s_gain   = gain;
    s_vol    = vol;
}

UiEvent ui_poll(void)
{
    // ── Consume encoder delta (disable IRQ briefly for atomic read) ──
    noInterrupts();
    int8_t delta = enc_delta;
    enc_delta = 0;
    interrupts();

    bool    btn_down    = (digitalRead(ENC_SW_PIN) == LOW);
    uint32_t now        = millis();

    // ── Detect short press and long press ────────────────────────────
    bool short_press = false;
    bool long_press  = false;

    if (btn_down && !enc_was_down)
    {
        enc_press_start = now;
        enc_was_down    = true;
    }
    else if (!btn_down && enc_was_down)
    {
        uint32_t held = now - enc_press_start;
        enc_was_down  = false;
        if (held >= ENC_LONG_PRESS_MS)
            long_press = true;
        else
            short_press = true;
    }

    // ── Route input by screen ────────────────────────────────────────
    bool dirty = false;

    if (s_screen == SCREEN_HOME)
    {
        if (delta != 0)
        {
            if (s_edit_vol)
            {
                s_vol += delta * kStep;
                if (s_vol < 0.0f) s_vol = 0.0f;
                if (s_vol > 1.0f) s_vol = 1.0f;
                dirty = true;
                s_pending_event = UI_VOL_CHANGED;
            }
            else
            {
                s_gain += delta * kStep;
                if (s_gain < 0.0f) s_gain = 0.0f;
                if (s_gain > 1.0f) s_gain = 1.0f;
                dirty = true;
                s_pending_event = UI_GAIN_CHANGED;
            }
        }
        if (short_press)
        {
            // Toggle between adjusting gain and volume
            s_edit_vol = !s_edit_vol;
            dirty = true;
        }
        if (long_press)
        {
            s_screen = SCREEN_MODEL_SELECT;
            dirty = true;
        }
        if (!short_press)
        {
            // short_press on home = parameter switch, not bypass
            // bypass mapped to a separate event if needed; keep simple:
            // separate bypass toggle: double short press not implemented —
            // user can add a dedicated bypass button via ENC_SW long-press
            // pattern or add a second GPIO.  For now long-press = models.
        }
    }
    else  // SCREEN_MODEL_SELECT
    {
        if (delta != 0 && s_list_count > 0)
        {
            int16_t next = (int16_t)s_sel_idx + delta;
            if (next < 0) next = 0;
            if (next >= (int16_t)s_list_count) next = s_list_count - 1;
            s_sel_idx = (uint8_t)next;

            // Scroll window
            if (s_sel_idx < s_scroll_top)
                s_scroll_top = s_sel_idx;
            else if (s_sel_idx >= s_scroll_top + kVisibleRows)
                s_scroll_top = s_sel_idx - kVisibleRows + 1;

            dirty = true;
        }
        if (short_press && s_list_count > 0)
        {
            s_load_index    = s_sel_idx;
            s_screen        = SCREEN_HOME;
            s_pending_event = UI_LOAD_MODEL;
            dirty = true;
        }
        if (long_press)
        {
            s_screen = SCREEN_HOME;
            dirty = true;
        }
    }

    if (dirty)
        redraw();

    // Return and clear pending event
    UiEvent ev   = s_pending_event;
    s_pending_event = UI_NONE;
    return ev;
}

uint8_t ui_selected_index(void) { return s_load_index; }
float   ui_gain(void)           { return s_gain; }
float   ui_vol(void)            { return s_vol; }
