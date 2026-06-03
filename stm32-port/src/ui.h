#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "model_mgr.h"

// UI events returned by ui_poll() each loop iteration.
enum UiEvent {
    UI_NONE          = 0,
    UI_LOAD_MODEL,       // user selected a model; ui_selected_index() has the index
    UI_TOGGLE_BYPASS,    // short-press on home screen
    UI_GAIN_CHANGED,     // gain knob moved; new value via ui_gain()
    UI_VOL_CHANGED,      // volume knob moved; new value via ui_vol()
};

// Call once in setup() after Wire.begin().
void ui_init(void);

// Feed the model list so the model-select screen can display filenames.
// Call whenever the list is (re)scanned.
void ui_set_model_list(const ModelEntry* list, uint8_t count);

// Update the home screen state (call each time something changes).
void ui_set_status(const char* model_name, bool active, float gain, float vol);

// Call every loop iteration; returns the highest-priority pending event.
UiEvent ui_poll(void);

// Index of the model the user selected (valid after UI_LOAD_MODEL).
uint8_t ui_selected_index(void);

float ui_gain(void);
float ui_vol(void);
