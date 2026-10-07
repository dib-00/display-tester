# Display Tester: reverse-engineering feature-phone TFT displays

Cheap replacement LCDs for feature phones (Nokia 105, "universal" 20-pin and 37-pin panels) are easy to buy but almost never come with a correct pinout. This repository documents how I identified, wired and drove four of these panels with ESP32 and ESP32-C3 boards. Every pinout here was **measured and verified on real hardware**.

**Author:** Dibyendu Mondal · [github.com/dib-00](https://github.com/dib-00)

## Displays

| Display | Size / resolution | Interface | Controller | Verified on | Folder |
|---|---|---|---|---|---|
| **KT-018 universal 20-pin** | 1.8" · 128×160 | 8-bit 8080 parallel (data bus reversed) | DCS / ST7735-style (ID not readable) | ESP32 NodeMCU-32S | [displays/KT-018-20pin](displays/KT-018-20pin) |
| **L-28LS124-01 37-pin** | 2.8" · 240×320 | 8-bit 8080 parallel | ST7789V (`85 85 52`) | ESP32 NodeMCU-32S | [displays/L-28LS124-01-37pin](displays/L-28LS124-01-37pin) |
| **KT-180N105new 12-pin** (Nokia 105) | 1.8" · 128×160 | 3-wire 9-bit SPI (no DC pin) | ST7735S (`7C 89 F0`) | ESP32-C3 SuperMini | [displays/KT-180N105new-12pin](displays/KT-180N105new-12pin) |
| **KT-018N105-2023 16-pin** (Nokia 105 2023) | 1.8" · 128×160 | 4-wire SPI (write-only, mounted 180°) | ST7735-compatible (ID not readable) | ESP32-C3 SuperMini | [displays/KT-018N105-2023-16pin](displays/KT-018N105-2023-16pin) |

Each display folder contains:

- **README:** specifications, full pinout table, multimeter signature, wiring for ESP32 / ESP32-C3 plus suggested maps for other MCUs, verified init sequence, issues and how they were solved, and troubleshooting
- **images/:** photos of the panel, the flex marking and test screens
- **firmware/:** PlatformIO projects with the display driver: a test program and/or a Wi-Fi GIF/image player

## Tools

| Tool | Purpose |
|---|---|
| [tools/spi-pin-scanner](tools/spi-pin-scanner) | Finds the SCK / SDA / CS / RESET / DC pins of an unknown SPI panel. Supports 4-wire and 3-wire modes, read-back detection, and a visual mode for write-only panels. |

The 8-bit parallel scanner, which brute-forces the CS / RS / WR / RD / RESET order, is part of [displays/KT-018-20pin/firmware/esp32-parallel-test](displays/KT-018-20pin/firmware/esp32-parallel-test).

## Wi-Fi GIF / image player

Three of the displays have a player project:

1. The board starts a Wi-Fi hotspot that serves a web page.
2. You upload a **GIF, PNG, JPG, WebP or BMP** from a phone or PC.
3. The browser decodes the GIF frames, crops, fits or rotates the image to the panel resolution, and converts it to RGB565.
4. The board stores the result in flash, so it survives power-off, and plays it.

The GIF decoder in the page was checked pixel-for-pixel against a reference decoder on animated, optimized (partial-frame) and interlaced GIFs.

## How the pinouts were found

1. **Beep test:** find the GND pins and the joined backlight pins.
2. **Diode mode, red probe on GND:**
   - about 0.45–0.55 V marks a **power** pin;
   - about 0.85–0.96 V marks a **signal** pin;
   - OL means unconnected.

   A run of 8 identical signal readings is a parallel data bus. 4–6 signal pins means SPI.
3. **Pull-up / pull-down test** from the MCU: a pin that reads the same either way is an **output** of the panel (for example TE), so it must never be driven.
4. **Read-back scan:** try every role order and see whether the panel drives the data line in reply.
5. **Visual scan** for write-only panels: each candidate draws its own number on screen. Use markers that reveal 180° rotation. On one panel, "091" seen upside-down read as "160".
6. **Confirm** with a colour cycle and an alignment screen (1 px border, coloured corners).

## Hardware notes

| Board | Notes |
|---|---|
| **ESP32 (WROOM-32)** | GPIO 34–39 are input-only. GPIO 0, 2, 12 and 15 are strapping pins: a panel holding them can block uploads (fix: hold BOOT, tap EN, release BOOT). |
| **ESP32-C3 SuperMini** | GPIO 8 is the LED and GPIO 9 is BOOT. On the board used here, GPIO 2 was stuck at 3.3 V, so check yours. |
| **Backlights** | Always fit a series resistor. A heavy backlight on the board's 3.3 V regulator can reset the MCU. |

## Building

All firmware uses [PlatformIO](https://platformio.org/):

```bash
cd displays/<display>/firmware/<project>
pio run -t upload --upload-port COMx
```
