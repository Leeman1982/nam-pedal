#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "model_mgr.h"

enum UiEvent {
    UI_NONE          = 0,
    UI_LOAD_MODEL,       // model selected; index via ui_selected_index()
    UI_TOGGLE_BYPASS,    // double-click on home screen
    UI_GAIN_CHANGED,     // encoder turned while gain selected; value via ui_gain()
    UI_VOL_CHANGED,      // encoder turned while vol selected; value via ui_vol()
};

// Call once in setup() after Wire.begin().
void ui_init(void);

// Feed the model list (call after SD scan or rescan).
void ui_set_model_list(const ModelEntry* list, uint8_t count);

// Update home screen state — call whenever model/gain/vol/bypass changes.
// Does NOT immediately redraw; the next ui_poll() will pick it up.
void ui_set_status(const char* model_name, bool active, float gain, float vol);

// Force a redraw on the next ui_poll() call.
void ui_force_redraw(void);

// Process encoder + button; returns the highest-priority pending event.
// Call every loop iteration (non-blocking).
UiEvent ui_poll(void);

uint8_t ui_selected_index(void);
float   ui_gain(void);
float   ui_vol(void);
