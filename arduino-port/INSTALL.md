# NAMPedalSTM32 — Arduino IDE Setup Guide

## Hardware

| Component | Notes |
|-----------|-------|
| **WeAct STM32F405RGT6** core board | 168 MHz Cortex-M4F, 1 MB Flash, 192 KB RAM |
| **PCM5102A / GY-PCM5102** | I2S DAC for audio output (see wiring below) |
| **EstarDyn 1.3" OLED module** | SH1106, I2C 0x3C, includes EC11 rotary encoder |
| Micro SD card (FAT32) | Contains `.namb` model files |
| Guitar input circuit | Voltage divider + DC-block cap (see Wiring section) |

---

## Step 1 — Arduino IDE & board package

1. Install **Arduino IDE 2.x** from https://www.arduino.cc/en/software
2. Open *File → Preferences* and add to **Additional boards manager URLs**:
   ```
   https://github.com/stm32duino/BoardManagerFiles/raw/main/package_stmicroelectronics_index.json
   ```
3. *Tools → Board → Boards Manager* → search **STM32** → install  
   **"STM32 MCU based boards" by STMicroelectronics** (v2.7+)
4. Select board: *Tools → Board → STM32 boards groups → Generic STM32F4 series*
5. Select part: *Tools → Board part number → Generic F405RGTx*
6. Upload method: *Tools → Upload method → STM32CubeProgrammer (SWD)*  
   *(or DFU if you prefer USB — hold BOOT0 during reset)*

---

## Step 2 — Install libraries via Library Manager

*Sketch → Include Library → Manage Libraries*

| Library | Author | Version |
|---------|--------|---------|
| **audio-tools** | Phil Schatzmann | ≥ 0.9.8 |
| **U8g2** | Oliver Kraus | ≥ 2.34 |
| **SD** | Arduino | built-in |

> If `audio-tools` does not appear, add the GitHub URL in Library Manager:
> `https://github.com/pschatzmann/arduino-audio-tools`

---

## Step 3 — Initialise git submodules

```bash
# From the repository root:
git submodule update --init --recursive
```

---

## Step 4 — Create the NAMCore Arduino library

```bash
cd arduino-port
./setup_arduino_libs.sh
```

This creates `~/Arduino/libraries/NAMCore/` with symlinks into the  
`NeuralAmpModelerCore` and `nam-binary-loader` submodule directories.

**Windows users:** Run the script in WSL, or manually create junctions with  
`mklink /J` pointing to the same targets.

---

## Step 5 — Arduino IDE compiler flags

The NAM engine and Eigen BLAS kernel need a few extra flags.  
Create (or edit) `~/.arduino15/preferences.txt` and add:

```
compiler.cpp.extra_flags=-O3 -ffast-math -funroll-loops -ftree-vectorize -fexceptions -DNAM_SAMPLE_FLOAT -DNAM_USE_INLINE_GEMM
```

Alternatively in Arduino IDE 2.x: *Sketch → Edit Sketch* then add a  
`sketch.yaml` (IDE 2.2+):

```yaml
profiles:
  default:
    fqbn: STMicroelectronics:stm32:GenF4:pnum=GENERIC_F405RGTX,opt=o3std,xserial=generic,usb=none,xusb=FS,upload_method=swdMethod
```

---

## Step 6 — Prepare SD card models

1. Format a micro SD card as **FAT32**.
2. Convert NAM `.nam` files to `.namb`:
   ```bash
   cd nam-binary-loader
   mkdir build && cd build
   cmake .. -DNAM_CORE_PATH=../../NeuralAmpModelerCore
   make
   ./nam2namb /path/to/model.nam model.namb
   ```
3. Copy `*.namb` files to the **root** of the SD card.  
   Only **nano** and some **feather** architectures meet the 1 ms deadline.

---

## Step 7 — Open and flash

1. Open `arduino-port/NAMPedalSTM32/NAMPedalSTM32.ino` in Arduino IDE.
2. Verify the sketch compiles (*Sketch → Verify/Compile*) — fix any missing  
   library errors before flashing.
3. Flash via SWD (ST-Link v2 or compatible) or USB DFU.

---

## Wiring

### Guitar ADC input (PA1)

```
Guitar tip ──[1 MΩ]──┬──[100 nF]──── PA1
                     │
                   [100 kΩ]  ← to 3V3 (bias mid-rail)
                   [100 kΩ]  ← to GND
```

> Never exceed 0 V–3.3 V on PA1. Use only the divider above — no direct  
> connection to line-level or instrument amplifier outputs.

### PCM5102A

| PCM5102A | STM32F405 | Note |
|----------|-----------|------|
| BCK | PB13 | I2S2_CK |
| LRCK | PB12 | I2S2_WS |
| DIN | PB15 | I2S2_SD |
| VCC | 3V3 | |
| GND | GND | |
| FMT | GND | I2S mode |
| DEMP | GND | de-emphasis off |
| XSMT | 3V3 | unmute |
| SCK | GND/float | not needed |

### SH1106 OLED + rotary encoder (EstarDyn module)

| Module | STM32F405 | Note |
|--------|-----------|------|
| SDA | PB9 | Wire default |
| SCL | PB8 | Wire default |
| VCC | 3V3 | |
| GND | GND | |
| CLK (A) | PC6 | EXTI interrupt |
| DT (B) | PC7 | sampled in ISR |
| SW | PC8 | button, active LOW |

### SD card (SPI1)

| SD module | STM32F405 |
|-----------|-----------|
| CS | PA4 |
| SCK | PA5 |
| MISO | PA6 |
| MOSI | PA7 |
| VCC | 3V3 |
| GND | GND |

---

## UI controls

| Action | Result |
|--------|--------|
| Rotate | Adjust selected parameter (GAIN or VOL) |
| Single click | Switch between GAIN / VOL |
| **Double-click** | **Toggle bypass** |
| Long press (>600 ms) | Open / close model-select screen |
| Rotate (in list) | Scroll model list |
| Single click (in list) | Load selected model |

The last loaded model name is saved to `/nam_cfg.txt` on the SD card  
and restored on next power-on.

---

## Troubleshooting

| Symptom | Likely cause | Fix |
|---------|-------------|-----|
| No sound at all | PCM5102A XSMT not tied to 3V3 | Check DAC mute pin |
| Clean passthrough only | No `.namb` on SD, or load failed | Check Serial output at 115200 baud |
| OLED blank | Wrong I2C address or SDA/SCL swapped | Run I2C scanner sketch |
| Crackle / artifacts | ADC input not biased to 1.65 V | Check voltage divider |
| Compile error re Eigen | NAMCore library not set up | Re-run `setup_arduino_libs.sh` |
| `audio-tools` I2S no output | PLLI2S not configured for HSE≠8 MHz | Adjust PLLI2SN/R in `NAMPedalSTM32.ino` |
