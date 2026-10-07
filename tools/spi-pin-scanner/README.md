# SPI pin scanner

This tool finds which signal pins of an unknown **serial (SPI) TFT** are SCK, SDA, CS, RESET and DC. It handles both **4-wire** (with a DC pin) and **3-wire 9-bit** (no DC) panels, including **write-only** panels that never answer a read.

It runs on an **ESP32-C3 SuperMini** (`env:c3`) or a **NodeMCU-32S** (`env:nodemcu32s`) and drives 6 unknown signal pins:

| Panel signal pin (KT-018N105-2023 numbering) | 2 | 6 | 7 | 8 | 9 | 10 |
|---|---|---|---|---|---|---|
| ESP32-C3 GPIO | 4 | 5 | 6 | 7 | 10 | 3 |
| ESP32 GPIO | 18 | 19 | 21 | 22 | 23 | 25 |

```bash
pio run -e c3 -t upload --upload-port COMx
```

## Serial commands (115200 baud)

| Command | What it does |
|---|---|
| `s` | **Read-back scan.** Classifies each pin with a pull-up / pull-down test (pins the panel drives are never driven). Then tries every role order in 4-wire and 3-wire mode and reads the controller ID on SDA and on any panel-output pin. A hit means the panel actively drove the line. |
| `v` | **Visual scan**, for write-only panels. Each candidate order initialises the panel and draws a red screen with its 3-digit number. Watch the screen and note the number that appears. |
| `r<a> <b> <ms>` | Replay candidates `a`…`b`, holding each for `ms` milliseconds |
| `L<a> <b> <t>` | Loop: candidates `a`…`b`, then candidate `t`, repeated until any key is sent |
| `k<n> <madctl> <spare>` | Confirm candidate `n`: colour cycle, then an alignment screen. `<spare>` holds unused pins: 0 = low, 1 = high, 2 = floating. |
| `a<n>` | Bit-alignment sweep for 3-wire candidates (0–8 extra clocks) |
| `f<n> <spare>` | Candidate `n` with the full ST7735R init |
| `V` | Targeted variants for the CS / DC question on two pins |
| `FC0` | The verified KT-018N105-2023 pinout: colour cycle, then the alignment screen |

## Method

1. **Measure first** (diode mode, red probe on GND):
   - about 0.5 V marks a power pin;
   - about 0.9 V marks a signal pin;
   - a beep marks GND.

   Power every power pin. An unpowered supply pin reads 0 V.
2. **Run `s`.** If the panel answers, the ID tells you the controller, and the hit gives the pinout.
3. **If nothing answers, run `v`** and watch for a number. Confirm it with `k`.
4. **Watch for rotated panels.** A 180°-mounted panel turns "091" into "160". Confirm with the corner colours of the alignment screen (`k`/`F`).
