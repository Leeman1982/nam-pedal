#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "config.h"

// One entry in the model list scanned from the SD card root.
struct ModelEntry {
    char name[64];   // filename only (no path), null-terminated
};

// Scans the SD card root for *.namb files and populates the list.
// Returns the number of models found.
uint8_t model_scan(ModelEntry* list, uint8_t max_count);

// Loads the .namb file at `path` (SD root, e.g. "/nano_fender.namb")
// into `buf` (must be MODEL_MAX_FILE_BYTES bytes).
// Returns the number of bytes read, or 0 on error.
size_t model_load_file(const char* path, uint8_t* buf, size_t buf_size);

// Writes the current model filename to the config file on SD so it
// survives power cycles.
void model_save_config(const char* filename);

// Reads the previously saved model filename into `out` (max `out_size` bytes).
// Returns true if a saved filename was found.
bool model_load_config(char* out, size_t out_size);
