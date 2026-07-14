// ui.cpp — SH1106 OLED (U8g2) + EC11 rotary encoder UI.
//
// HOME screen:
//   Rotate        → adjust selected parameter (GAIN or VOL)
//   Single click  → toggle GAIN / VOL selection
//   Double-click  → toggle bypass (UI_TOGGLE_BYPASS)
//   Long press    → open model-select list
//
// MODEL_SELECT screen:
//   Rotate        → scroll list
//   Single click  → load selected model (UI_LOAD_MODEL)
//   Long press    → back to home

#include "ui.h"
#include "config.h"
#include <Arduino.h>
#include <Wire.h>
#include <U8g2lib.h>
#include <string.h>
#include <stdio.h>

// ─── OLED ─────────────────────────────────────────────────────────────────────
// SDA→PB9, SCL→PB8 (Wire defaults for STM32duino F4).
static U8G2_SH1106_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);

// ─── Encoder ──────────────────────────────────────────────────────────────────
// int16_t avoids overflow if encoder is spun quickly between polls.
static volatile int16_t enc_delta = 0;

static void enc_clk_isr(void)
{
    // Quadrature: sample DT when CLK falls.
    enc_delta += digitalRead(ENC_DT_PIN) ? +1 : -1;
}

// ─── Button state machine ─────────────────────────────────────────────────────
static uint32_t btn_press_start  = 0;
static uint32_t btn_last_release = 0;
static uint8_t  btn_click_count  = 0;
static bool     btn_was_down     = false;

// ─── Model list ───────────────────────────────────────────────────────────────
static const ModelEntry* s_list       = nullptr;
static uint8_t           s_list_count = 0;

// ─── Home screen state ────────────────────────────────────────────────────────
static char  s_model_name[64] = "none";
static bool  s_active         = false;
static float s_gain           = 0.5f;
static float s_vol            = 1.0f;

// ─── Navigation state ─────────────────────────────────────────────────────────
enum Screen { SCREEN_HOME, SCREEN_MODEL_SELECT };
static Screen  s_screen     = SCREEN_HOME;
static uint8_t s_sel_idx    = 0;
static uint8_t s_scroll_top = 0;
static bool    s_edit_vol   = false;
static bool    s_dirty      = false;

static UiEvent s_pending_event = UI_NONE;
static uint8_t s_load_index    = 0;

static constexpr float   kStep        = 0.025f;
static constexpr uint8_t kVisibleRows = 3;

// ─── Draw helpers ─────────────────────────────────────────────────────────────

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
    u8g2.setDrawColor(1);   // reset — may have been 0 from model-select

    // Status bar
    u8g2.setFont(u8g2_font_5x7_tr);
    u8g2.drawStr(0, 7, "NAM Pedal");
    const char* st = s_active ? "[ON]" : "[BYP]";
    u8g2.drawStr(128 - (int)u8g2.getStrWidth(st), 7, st);
    u8g2.drawHLine(0, 9, 128);

    // Model name — truncate to fit
    char trunc[22];
    snprintf(trunc, sizeof(trunc), "%.21s", s_model_name);
    u8g2.drawStr(0, 20, trunc);

    // Gain row
    bool gain_sel = !s_edit_vol;
    u8g2.setFont(u8g2_font_5x7_tr);
    u8g2.drawStr(0, 33, gain_sel ? ">GAIN" : " GAIN");
    draw_bar(36, 26, 68, 8, s_gain);
    char val[8];
    snprintf(val, sizeof(val), "%.2f", s_gain);
    // Right-align the value label
    u8g2.drawStr(128 - (int)u8g2.getStrWidth(val), 33, val);

    // Volume row
    bool vol_sel = s_edit_vol;
    u8g2.drawStr(0, 47, vol_sel ? ">VOL " : " VOL ");
    draw_bar(36, 40, 68, 8, s_vol);
    snprintf(val, sizeof(val), "%.2f", s_vol);
    u8g2.drawStr(128 - (int)u8g2.getStrWidth(val), 47, val);

    // Hint line
    u8g2.setFont(u8g2_font_4x6_tr);
    u8g2.drawStr(0, 63, "CLK:sel  DBL:byp  HOLD:list");

    u8g2.sendBuffer();
}

static void draw_model_select(void)
{
    u8g2.clearBuffer();
    u8g2.setDrawColor(1);

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

            uint8_t y        = 21 + r * 14;
            bool    selected = (idx == s_sel_idx);

            if (selected)
            {
                u8g2.drawBox(0, y - 10, 128, 12);
                u8g2.setDrawColor(0);
            }
            char row[22];
            snprintf(row, sizeof(row), "%.21s", s_list[idx].name);
            u8g2.drawStr(4, y, row);
            if (selected)
                u8g2.setDrawColor(1);
        }
    }

    u8g2.setFont(u8g2_font_4x6_tr);
    char footer[36];
    snprintf(footer, sizeof(footer), "%u/%u  CLK:load  HOLD:back",
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
    Wire.setClock(400000);
    u8g2.begin();

    pinMode(ENC_CLK_PIN, INPUT_PULLUP);
    pinMode(ENC_DT_PIN,  INPUT_PULLUP);
    pinMode(ENC_SW_PIN,  INPUT_PULLUP);
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
    s_dirty  = true;   // trigger redraw on next poll
}

void ui_force_redraw(void)
{
    s_dirty = true;
}

UiEvent ui_poll(void)
{
    // Atomically consume encoder delta
    noInterrupts();
    int16_t delta = enc_delta;
    enc_delta = 0;
    interrupts();

    bool     btn_down = (digitalRead(ENC_SW_PIN) == LOW);
    uint32_t now      = millis();

    // ── Button state machine ──────────────────────────────────────────────────
    bool single_click = false;
    bool long_press   = false;

    if (btn_down && !btn_was_down)
    {
        btn_press_start = now;
        btn_was_down    = true;
    }
    else if (!btn_down && btn_was_down)
    {
        uint32_t held = now - btn_press_start;
        btn_was_down  = false;

        if (held >= ENC_LONG_PRESS_MS)
        {
            long_press      = true;
            btn_click_count = 0;  // discard pending single-click
        }
        else
        {
            btn_click_count++;
            btn_last_release = now;
        }
    }

    // Decide if we have a confirmed single or double click
    // (wait ENC_DBLCLICK_MS after last release before committing single)
    bool double_click = false;
    if (btn_click_count >= 2)
    {
        double_click    = true;
        btn_click_count = 0;
    }
    else if (btn_click_count == 1 &&
             (now - btn_last_release) >= ENC_DBLCLICK_MS)
    {
        single_click    = true;
        btn_click_count = 0;
    }

    // ── Route by screen ───────────────────────────────────────────────────────
    if (s_screen == SCREEN_HOME)
    {
        if (delta != 0)
        {
            if (s_edit_vol)
            {
                s_vol += delta * kStep;
                if (s_vol < 0.0f) s_vol = 0.0f;
                if (s_vol > 1.0f) s_vol = 1.0f;
                s_pending_event = UI_VOL_CHANGED;
            }
            else
            {
                s_gain += delta * kStep;
                if (s_gain < 0.0f) s_gain = 0.0f;
                if (s_gain > 1.0f) s_gain = 1.0f;
                s_pending_event = UI_GAIN_CHANGED;
            }
            s_dirty = true;
        }

        if (single_click)
        {
            s_edit_vol = !s_edit_vol;
            s_dirty    = true;
        }

        if (double_click)
        {
            s_pending_event = UI_TOGGLE_BYPASS;
            s_dirty         = true;
        }

        if (long_press)
        {
            s_screen = SCREEN_MODEL_SELECT;
            s_dirty  = true;
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

            if (s_sel_idx < s_scroll_top)
                s_scroll_top = s_sel_idx;
            else if (s_sel_idx >= s_scroll_top + kVisibleRows)
                s_scroll_top = s_sel_idx - kVisibleRows + 1;

            s_dirty = true;
        }

        if (single_click && s_list_count > 0)
        {
            s_load_index    = s_sel_idx;
            s_screen        = SCREEN_HOME;
            s_pending_event = UI_LOAD_MODEL;
            s_dirty         = true;
        }

        if (long_press)
        {
            s_screen = SCREEN_HOME;
            s_dirty  = true;
        }
    }

    if (s_dirty)
    {
        s_dirty = false;
        redraw();
    }

    UiEvent ev      = s_pending_event;
    s_pending_event = UI_NONE;
    return ev;
}

uint8_t ui_selected_index(void) { return s_load_index; }
float   ui_gain(void)           { return s_gain; }
float   ui_vol(void)            { return s_vol; }
