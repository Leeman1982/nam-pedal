# Build & Flash Guide — NAM Pedal STM32F405
### Plain-English, step by step. No experience needed.

---

## What you need before you start

**Software (free):**
- A computer running Windows, Mac, or Linux
- Internet connection
- Git (https://git-scm.com/downloads — click the installer for your OS)
- Arduino IDE 2 (you'll install this in Step 1)
- STM32CubeProgrammer (you'll install this in Step 3)
- CMake (you'll install this in Step 6) — only needed once to convert model files

**Hardware:**
- WeAct STM32F405RGT6 board
- USB-C cable (the one that came with the board)
- PCM5102A DAC module (GY-PCM5102)
- EstarDyn 1.3" OLED + rotary encoder module
- Micro SD card (any size, you'll format it)
- A breadboard and some jumper wires

---

## Step 1 — Install Arduino IDE 2

1. Go to **https://www.arduino.cc/en/software**
2. Download **Arduino IDE 2** for your operating system (the big green button)
3. Run the installer — accept all defaults, click Next/Install throughout
4. Open Arduino IDE when it finishes

---

## Step 2 — Add the STM32 board package

This teaches Arduino IDE about STM32 chips.

1. In Arduino IDE, click **File → Preferences**  
   *(on Mac: Arduino IDE → Settings)*
2. Find the box labelled **"Additional boards manager URLs"**
3. Click the small icon to the right of that box (looks like two windows)
4. Paste this URL on a new line:
   ```
   https://github.com/stm32duino/BoardManagerFiles/raw/main/package_stmicroelectronics_index.json
   ```
5. Click **OK**, then **OK** again

Now install the package:

6. Click **Tools → Board → Boards Manager**
7. In the search box type: `STM32`
8. Find **"STM32 MCU based boards"** by STMicroelectronics
9. Click **Install** — this downloads ~200 MB, wait for it to finish
10. Close the Boards Manager

---

## Step 3 — Install STM32CubeProgrammer

This is the tool that actually puts the code onto the chip.

1. Go to **https://www.st.com/en/development-tools/stm32cubeprog.html**
2. Click **Get Software** → create a free account if asked → download
3. Run the installer — accept all defaults
4. **Windows only:** The installer will prompt to install a USB driver.  
   Click **Yes / Install** when it asks. This is required for DFU flashing.

---

## Step 4 — Install the three required libraries

Back in Arduino IDE:

1. Click **Sketch → Include Library → Manage Libraries**
2. Search for **`audio-tools`** → find the one by **Phil Schatzmann** → click **Install**  
   *(If it asks to install dependencies, click "Install All")*
3. Search for **`U8g2`** → find the one by **Oliver Kraus** → click **Install**
4. Search for **`SD`** → find **"SD" by Arduino** → click **Install** (may already be installed)
5. Close the Library Manager

---

## Step 5 — Download the project code

Open a terminal (on Windows: right-click the Desktop → "Open in Terminal" or use Git Bash).

```bash
git clone --recursive https://github.com/Leeman1982/nam-pedal.git
cd nam-pedal
git checkout claude/stm32-nam-a2-port-8fUnS
git submodule update --init --recursive
```

> **What `--recursive` does:** The project uses two sub-projects (NAM engine and
> binary loader). `--recursive` downloads them automatically alongside the main code.
> The final `submodule update` command makes sure they're fully up to date.

---

## Step 6 — Set up the NAM engine as an Arduino library

The NAM inference engine needs to be installed where Arduino IDE can find it.

**Mac / Linux:**
```bash
cd arduino-port
./setup_arduino_libs.sh
```

**Windows (run in Git Bash or WSL):**
```bash
cd arduino-port
bash setup_arduino_libs.sh
```

You should see: `NAMCore library created at: .../Arduino/libraries/NAMCore`

> If you get a "permission denied" error on Mac/Linux:
> `chmod +x arduino-port/setup_arduino_libs.sh` then try again.

---

## Step 7 — Open the sketch in Arduino IDE

1. In Arduino IDE click **File → Open**
2. Navigate to the folder where you cloned the project
3. Open: `arduino-port / NAMPedalSTM32 / NAMPedalSTM32.ino`

The sketch opens showing several tabs (config.h, audio.cpp, ui.cpp, etc.) — that's normal.

---

## Step 8 — Configure the board settings

In Arduino IDE, set these options under the **Tools** menu, one by one:

| Menu item | What to select |
|-----------|---------------|
| **Board** | STM32 boards groups → Generic STM32F4 series |
| **Board part number** | Generic F405RGTx |
| **U(S)ART support** | Enabled (generic Serial) |
| **USB support** | None |
| **Optimize** | Fastest (-O3) |
| **Upload method** | STM32CubeProgrammer (DFU) |

> **Can't find these menus?** You must select the board first — the extra options
> only appear after you've chosen "Generic STM32F4 series".

---

## Step 9 — Compile (verify it builds before touching the hardware)

Click the **✓ Verify** button (tick icon, top-left of Arduino IDE).

This will take 2–5 minutes the first time — it's compiling the NAM neural network engine.  
At the end you should see: **"Compilation complete."** at the bottom of the screen.

**If you get errors:**

| Error message contains | Fix |
|------------------------|-----|
| `No such file: NAM/dsp.h` | Re-run `setup_arduino_libs.sh` (Step 6) |
| `No such file: AudioTools.h` | Install `audio-tools` library (Step 4) |
| `No such file: U8g2lib.h` | Install `U8g2` library (Step 4) |
| Any other red error | Post the full error text to the project GitHub issues |

---

## Step 10 — Prepare the SD card

1. Format your micro SD card as **FAT32**  
   *(Windows: right-click the card in Explorer → Format → FAT32 → Start)*  
   *(Mac: Disk Utility → select card → Erase → MS-DOS FAT)*

2. **Factory presets are already included!** The repo contains 10 ENGL Powerball II captures
   ready to copy straight to your SD card — no conversion needed:

   | File | What it sounds like |
   |------|-------------------|
   | `engl_pb2_clean.namb` | Clean channel |
   | `engl_pb2_crunch.namb` | Crunch channel |
   | `engl_pb2_crunch_boost.namb` | Crunch + boost |
   | `engl_pb2_hg_rhythm.namb` | High-gain rhythm |
   | `engl_pb2_hg_rhy_boost.namb` | High-gain rhythm + boost |
   | `engl_pb2_hg_lead.namb` | High-gain lead |
   | `engl_pb2_muff1.namb` | Clean + EHX Big Muff (setting 1) |
   | `engl_pb2_muff2.namb` | Clean + EHX Big Muff (setting 2) |
   | `engl_pb2_rat_dist.namb` | Clean + ProCo RAT2 (distortion) |
   | `engl_pb2_rat_doom.namb` | Clean + ProCo RAT2 (doom) |

   These files are in the `arduino-port/presets/` folder in the repo.

3. Get more NAM model files (optional). Download `.nam` files from sites like **ToneHunt** or **Tone3000**,
   then convert them. Only **nano** or **feather** size models will run fast enough on this hardware.

   **Converting `.nam` → `.namb`** (do this once per model):
   ```bash
   # From inside the project folder:
   cd nam-binary-loader
   mkdir build && cd build
   cmake .. -DNAM_CORE_PATH=../../NeuralAmpModelerCore
   make
   # Now convert a model:
   ./nam2namb /path/to/your-model.nam my-model.namb
   ```
   > **Windows:** Run these commands in WSL (Windows Subsystem for Linux),
   > or install CMake + MinGW and run in the regular terminal.

4. Copy all `.namb` files to the **root** (top level) of the SD card  
   — not inside any folder, just straight onto the card.

5. Eject the SD card safely, then insert it into the WeAct board's SD slot.

---

## Step 11 — Wire everything up

Use jumper wires on a breadboard. **3.3 V only — never connect 5 V to any pin.**

### PCM5102A DAC (audio output to your amp/headphones)

| PCM5102A pin label | Connect to WeAct board pin |
|--------------------|---------------------------|
| BCK | PB13 |
| LCK | PB12 |
| DIN | PB15 |
| VCC | 3V3 |
| GND | GND |
| FMT | GND |
| DEMP | GND |
| XSMT | 3V3 ← **important: must be 3V3 or no sound** |
| SCK | GND (or leave unconnected) |

### OLED + encoder (EstarDyn module)

| Module pin | WeAct board pin |
|------------|----------------|
| SDA | PB9 |
| SCL | PB8 |
| VCC | 3V3 |
| GND | GND |
| CLK (encoder A) | PC6 |
| DT (encoder B) | PC7 |
| SW (encoder button) | PC8 |

### Guitar input (build this on a breadboard)

You need a small 3-resistor + 1-capacitor circuit to safely connect your guitar to the STM32's ADC pin. The guitar signal runs on ±1V and the ADC expects 0–3.3V — this circuit shifts it to the right range.

```
Guitar cable (tip) ─────┬──── 1 MΩ ──── GND
                        │
                      100 nF (capacitor, any direction for non-polar)
                        │
              ┌─────────┴─────────┐
            100 kΩ              100 kΩ
              │                   │
             GND                 3V3
              └────── PA1 ────────┘
                   (ADC input)
```

Simpler version if you only have a handful of resistors:
- Solder a 3.5mm jack to your breadboard
- Tip → 100nF cap → PA1
- PA1 also connects via 100kΩ to 3V3 **and** via 100kΩ to GND
- Guitar sleeve → GND

> **Never plug a guitar amp OUTPUT into this circuit.** Guitar pickups are fine,
> effects pedal outputs are fine, audio interfaces are fine at instrument level.

---

## Step 12 — Flash the firmware

### Put the board into DFU (firmware update) mode:

1. Plug the WeAct board into your computer via USB-C
2. Find the two small buttons on the board labelled **BOOT** and **RST** (Reset)
3. Press and **hold** the **BOOT** button
4. While still holding BOOT, press and release **RST**
5. Now release **BOOT**

The board is now in DFU mode. It will appear as a new USB device — **no LED activity**, that's normal.

### Upload from Arduino IDE:

With the board in DFU mode, click the **→ Upload** button (right-arrow icon) in Arduino IDE.

Arduino IDE will compile (if not already done) and then call STM32CubeProgrammer automatically to flash the chip. You'll see output like:

```
Flashing with a STM32CubeProgrammer (DFU)
...
File download complete
Time elapsed during download operation: ...
```

When it finishes, the board resets itself and starts running the firmware.

> **If upload fails with "No DFU device found":**
> - Make sure the USB driver installed correctly (Step 3)
> - Try the BOOT+RST sequence again
> - On Windows: open Device Manager and check for "STM32 BOOTLOADER" under USB devices

---

## Step 13 — First boot check

Open the Arduino IDE **Serial Monitor** (*Tools → Serial Monitor*, baud rate **115200**).

You should see something like:
```
Scanning SD...
3 model(s) found
Loading: nano_fender.namb
Read 1756 bytes
Model ready in 14 ms
Audio started
```

If you see **"0 model(s) found"** — the SD card isn't being read. Check PA4–PA7 wiring.  
If you see **"No models — bypass mode"** — SD card is working but no `.namb` files found.

The OLED should light up showing the NAM Pedal home screen with gain and volume bars.

---

## Step 14 — Play!

- Plug your guitar into the input circuit (PA1)
- Plug your amp or headphones into the PCM5102A output
- The display shows the loaded model, gain bar, and volume bar

### Controls:
| What you do | What happens |
|-------------|-------------|
| Turn the knob | Adjusts whichever parameter is highlighted (GAIN or VOL) |
| Click once | Switches highlight between GAIN and VOL |
| Click twice quickly | **Toggles bypass on/off** |
| Hold knob button >0.6 sec | Opens the model list |
| Turn knob (in list) | Scrolls through your `.namb` files |
| Click once (in list) | Loads the selected model |
| Hold button (in list) | Goes back to home screen |

The last model you selected is remembered even after power-off.

---

## Troubleshooting

| Problem | Most likely cause | Try this |
|---------|------------------|----------|
| No sound at all | XSMT pin on PCM5102A not wired to 3V3 | Check that connection first |
| Only clean guitar, no amp tone | No model loaded, or bypass is on | Double-click the encoder button |
| Crackling / buzzing | Input bias circuit wrong | Check the 100kΩ/100kΩ divider |
| OLED is blank | SDA/SCL swapped, or not getting 3V3 | Swap PB8/PB9 wires |
| Serial shows nothing | Wrong baud rate | Set Serial Monitor to 115200 |
| Upload fails "DFU not found" | USB driver missing or wrong BOOT sequence | Reinstall STM32CubeProgrammer and retry BOOT+RST |
| Compile error "NAM/dsp.h not found" | NAMCore library not set up | Run `setup_arduino_libs.sh` again |
| OLED shows "no model" | SD card not inserted or wrong format | Reformat as FAT32, re-add files |
