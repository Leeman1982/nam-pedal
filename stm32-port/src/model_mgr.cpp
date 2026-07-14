#include "model_mgr.h"
#include <Arduino.h>
#include <SD.h>
#include <SPI.h>
#include <string.h>
#include <strings.h>

// ─── SD card init (called lazily) ────────────────────────────────────────────

static bool s_sd_ready = false;

static bool ensure_sd(void)
{
    if (s_sd_ready) return true;
    // SPI1: SCK=PA5, MISO=PA6, MOSI=PA7, CS=PA4 (see config.h)
    SPI.setMOSI(SD_MOSI_PIN);
    SPI.setMISO(SD_MISO_PIN);
    SPI.setSCLK(SD_SCK_PIN);
    s_sd_ready = SD.begin(SD_CS_PIN);
    return s_sd_ready;
}

// ─── Scan SD root for *.namb files ───────────────────────────────────────────

static bool is_namb(const char* name)
{
    size_t len = strlen(name);
    if (len < 6) return false;
    // Case-insensitive compare of last 5 chars
    const char* ext = name + len - 5;
    return (ext[0] == '.' &&
            (ext[1] == 'n' || ext[1] == 'N') &&
            (ext[2] == 'a' || ext[2] == 'A') &&
            (ext[3] == 'm' || ext[3] == 'M') &&
            (ext[4] == 'b' || ext[4] == 'B'));
}

uint8_t model_scan(ModelEntry* list, uint8_t max_count)
{
    if (!ensure_sd()) return 0;

    File root = SD.open("/");
    if (!root) return 0;

    uint8_t count = 0;
    while (count < max_count)
    {
        File entry = root.openNextFile();
        if (!entry) break;

        const char* n = entry.name();
        // SD.h returns just the filename (no leading slash)
        if (!entry.isDirectory() && is_namb(n))
        {
            strncpy(list[count].name, n, sizeof(list[count].name) - 1);
            list[count].name[sizeof(list[count].name) - 1] = '\0';
            count++;
        }
        entry.close();
    }
    root.close();
    return count;
}

// ─── Load a model file into RAM ──────────────────────────────────────────────

size_t model_load_file(const char* path, uint8_t* buf, size_t buf_size)
{
    if (!ensure_sd()) return 0;

    File f = SD.open(path, FILE_READ);
    if (!f) return 0;

    size_t file_size = f.size();
    if (file_size == 0 || file_size > buf_size)
    {
        f.close();
        return 0;
    }

    // Read in 512-byte chunks (SD library is more reliable this way)
    size_t total = 0;
    while (total < file_size)
    {
        size_t chunk = file_size - total;
        if (chunk > 512) chunk = 512;
        int n = f.read(buf + total, chunk);
        if (n <= 0) break;
        total += (size_t)n;
    }
    f.close();
    return total;
}

// ─── Persist last-used model name ────────────────────────────────────────────

void model_save_config(const char* filename)
{
    if (!ensure_sd()) return;
    SD.remove(MODEL_CONFIG_FILE);
    File cfg = SD.open(MODEL_CONFIG_FILE, FILE_WRITE);
    if (cfg)
    {
        cfg.println(filename);
        cfg.close();
    }
}

bool model_load_config(char* out, size_t out_size)
{
    if (!ensure_sd()) return false;
    File cfg = SD.open(MODEL_CONFIG_FILE, FILE_READ);
    if (!cfg) return false;

    size_t i = 0;
    while (cfg.available() && i < out_size - 1)
    {
        char c = (char)cfg.read();
        if (c == '\n' || c == '\r') break;
        out[i++] = c;
    }
    out[i] = '\0';
    cfg.close();
    return i > 0;
}
