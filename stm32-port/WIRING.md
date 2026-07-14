# STM32F405 NAMPedal — Wiring Reference

## Board: WeAct Studio STM32F405RGT6 Core Board V1.1

---

## PCM5102A I2S DAC (audio output)

| PCM5102A pin | STM32F405 pin | Function          |
|-------------|---------------|-------------------|
| BCK         | PB13          | I2S2 bit clock    |
| LCK / LRCK  | PB12          | I2S2 word select  |
| DIN         | PB15          | I2S2 data         |
| VCC         | 3V3           |                   |
| GND         | GND           |                   |
| FMT         | GND           | I2S standard mode |
| DEMP        | GND           | de-emphasis off   |
| XSMT        | 3V3           | unmute            |
| SCK         | GND / float   | not needed        |

---

## Guitar ADC Input (PA1)

Build a simple input stage on stripboard:

```
Guitar (tip) ──[ 1 MΩ ]──┬──[ 100 nF ]──┬── PA1
                          │              │
                        [100 kΩ]       [100 kΩ]
                          │              │
                         GND            3V3
```

The 100 kΩ/100 kΩ divider biases the signal to 1.65 V mid-rail.
The 100 nF cap blocks DC from the guitar output.
The 1 MΩ is a guitar-impedance load to ground (prevents floating when unplugged).

> **ADC input range:** 0 V – 3.3 V only. Never connect directly to a line-level
> signal or a guitar amp output — always buffer with the divider above.

---

## SD Card (SPI1, onboard slot on WeAct F405)

If your board does **not** have an onboard SD slot, wire an external SPI SD module:

| SD module pin | STM32F405 pin | Notes               |
|--------------|---------------|---------------------|
| CS           | PA4           | SPI1_NSS (GPIO CS)  |
| SCK          | PA5           | SPI1_SCK            |
| MISO         | PA6           | SPI1_MISO           |
| MOSI         | PA7           | SPI1_MOSI           |
| VCC          | 3V3           |                     |
| GND          | GND           |                     |

---

## SH1106 OLED + Rotary Encoder (EstarDyn module)

I2C address of the OLED is **0x3C** (fixed on module).

| Module pin | STM32F405 pin | Function                |
|-----------|---------------|-------------------------|
| SDA       | PB9           | I2C1 SDA (Wire default) |
| SCL       | PB8           | I2C1 SCL (Wire default) |
| VCC       | 3V3           |                         |
| GND       | GND           |                         |

Rotary encoder (EC11):

| Encoder pin | STM32F405 pin | Notes                        |
|------------|---------------|------------------------------|
| CLK (A)    | PC6           | EXTI interrupt, INPUT_PULLUP |
| DT  (B)    | PC7           | sampled in ISR, INPUT_PULLUP |
| SW         | PC8           | button, INPUT_PULLUP, active LOW |

---

## Status LED

The WeAct F405 has an onboard user LED on **PC13** (active-low).  
Set `STATUS_LED_INVERT` in `config.h` to `false` if your variant is active-high.

---

## Power

Power the board from USB (5 V via the USB-C connector) or supply 5 V to the VB
pin.  The onboard 3V3 LDO provides power for the OLED and PCM5102A; total
current is well within 300 mA.

---

## SD Card Setup

Format the SD card as **FAT32**.  Place `.namb` model files in the **root
directory**. The firmware scans for all `*.namb` files at boot and lets you
select them via the OLED UI.

### Converting `.nam` → `.namb`

```bash
cd nam-binary-loader
mkdir build && cd build
cmake .. -DNAM_CORE_PATH=../../NeuralAmpModelerCore
make
./nam2namb /path/to/model.nam output.namb
cp output.namb /path/to/sdcard/
```

Only **nano** and some **feather** models will comfortably meet the 1 ms
audio deadline on the STM32F405 at 168 MHz.

---

## UI Quick Reference

| Action                | Result                          |
|-----------------------|---------------------------------|
| Rotate encoder        | Adjust selected parameter       |
| Short press           | Switch between Gain / Volume    |
| Long press (>600 ms)  | Toggle Home ↔ Model Select      |
| Rotate (in list)      | Scroll model list               |
| Short press (in list) | Load selected model             |
| Long press (in list)  | Back to Home screen             |
