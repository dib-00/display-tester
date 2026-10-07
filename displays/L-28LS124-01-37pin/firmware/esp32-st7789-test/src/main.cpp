// Author: Dibyendu Mondal (https://github.com/dib-00)
/**
 * F0-28LS124-01 (2.8", 240x320, ST7789V, 8-bit 8080 parallel) — display test
 * Board: NodeMCU-32S (ESP32-WROOM-32). Pin map: see platformio.ini / README.md.
 *
 * On boot:
 *   1. Classifies the five unidentified flex pins (LCD 9, 10, 11, 12, 26):
 *      pins the panel drives itself are outputs (e.g. TE) and are never driven.
 *   2. Searches strap levels for the remaining ones (likely IM mode pins),
 *      re-initialising and reading the controller ID for each combination,
 *      and keeps the first combination the panel answers to.
 *   3. Cycles through test patterns every 2.5 s.
 *
 * Serial commands (115200, single characters):
 *   n  next pattern         p  pause / resume cycling
 *   i  toggle inversion     r  rotate 90 degrees
 *   d  re-read controller ID    s  re-run the strap search
 *   m  GRAM write/read-back test (verifies pixel data reaches display memory)
 *   o  glass test: every 3 s toggles DISPLAY OFF <-> ON + black/red fill
 */

#include <Arduino.h>
#include <TFT_eSPI.h>

static TFT_eSPI tft;

static const uint32_t PATTERN_MS = 2500;
static bool paused = false;
static bool inverted = false;  // panel needs INVOFF (TFT_INVERSION_OFF)
static uint8_t rotation = 0;
static uint8_t patternIndex = 0;
static uint32_t lastSwitch = 0;

// ---------------------------------------------------------------------------
// Unidentified flex pins (LCD pin -> GPIO)
// ---------------------------------------------------------------------------

struct ExtraPin {
  uint8_t lcdPin;
  uint8_t gpio;
  bool panelDriven;  // panel drives it -> output (e.g. TE), never drive it
};

static ExtraPin extras[] = {{9, 18, false}, {10, 19, false}, {11, 21, false},
                            {12, 22, false}, {26, 23, false}};
static constexpr uint8_t EXTRA_COUNT = sizeof(extras) / sizeof(extras[0]);

static int8_t strapLevels[EXTRA_COUNT];  // -1 = floating, 0 = LOW, 1 = HIGH

static void classifyExtras() {
  Serial.println("Unidentified pins (pull-up / pull-down test):");
  for (auto &e : extras) {
    pinMode(e.gpio, INPUT_PULLUP);   delay(5); const int pu = digitalRead(e.gpio);
    pinMode(e.gpio, INPUT_PULLDOWN); delay(5); const int pd = digitalRead(e.gpio);
    pinMode(e.gpio, INPUT);
    e.panelDriven = (pu == pd);
    Serial.printf("  LCD %-2u (GPIO%-2u): pull-up=%d pull-down=%d -> %s\n", e.lcdPin, e.gpio, pu,
                  pd, e.panelDriven ? "DRIVEN BY PANEL (output, left alone)" : "input / floating");
  }
}

static void applyStraps() {
  for (uint8_t i = 0; i < EXTRA_COUNT; ++i) {
    if (strapLevels[i] < 0 || extras[i].panelDriven) {
      pinMode(extras[i].gpio, INPUT);
    } else {
      pinMode(extras[i].gpio, OUTPUT);
      digitalWrite(extras[i].gpio, strapLevels[i]);
    }
  }
}

static void printStraps() {
  for (uint8_t i = 0; i < EXTRA_COUNT; ++i) {
    const char *v = extras[i].panelDriven ? "out" : strapLevels[i] < 0 ? "Z" : strapLevels[i] ? "1" : "0";
    Serial.printf("LCD%u=%s ", extras[i].lcdPin, v);
  }
}

// ---------------------------------------------------------------------------
// Controller ID
// ---------------------------------------------------------------------------

static uint8_t reverseBits(uint8_t b) {
  b = (b & 0xF0) >> 4 | (b & 0x0F) << 4;
  b = (b & 0xCC) >> 2 | (b & 0x33) << 2;
  return (b & 0xAA) >> 1 | (b & 0x55) << 1;
}

enum class IdResult { NoReply, Unexpected, St7789, St7789Reversed };

// readcommand8(cmd, n) returns the n-th byte read after the command; the
// first byte after a read command is a dummy on the 8080 bus.
static IdResult readControllerId(bool verbose) {
  const uint8_t id1 = tft.readcommand8(0x04, 2);
  const uint8_t id2 = tft.readcommand8(0x04, 3);
  const uint8_t id3 = tft.readcommand8(0x04, 4);

  IdResult r;
  if (id1 == 0x85 && id2 == 0x85 && id3 == 0x52) r = IdResult::St7789;
  else if (reverseBits(id1) == 0x85 && reverseBits(id2) == 0x85 && reverseBits(id3) == 0x52)
    r = IdResult::St7789Reversed;
  else if ((id1 == id2 && id2 == id3) && (id1 == 0x00 || id1 == 0xFF)) r = IdResult::NoReply;
  else r = IdResult::Unexpected;

  if (verbose) {
    const uint8_t pm = tft.readcommand8(0x0A, 2);
    const uint8_t colmod = tft.readcommand8(0x0C, 2);
    Serial.printf("RDDID  = %02X %02X %02X  %s\n", id1, id2, id3,
                  r == IdResult::St7789           ? "(ST7789V confirmed)"
                  : r == IdResult::St7789Reversed ? "(ST7789V, but DATA BUS IS REVERSED: swap DB8..DB15 order)"
                  : r == IdResult::NoReply        ? "(no reply)"
                                                  : "(unexpected ID, but the panel is answering)");
    Serial.printf("RDDPM  = %02X (expect 9C after init), COLMOD = %02X (expect x5)\n", pm, colmod);
  }
  return r;
}

static bool initAndProbe() {
  applyStraps();
  delay(5);
  tft.init();
  const IdResult r = readControllerId(false);
  printStraps();
  Serial.printf("-> %s\n", r == IdResult::St7789           ? "ST7789V"
                           : r == IdResult::St7789Reversed ? "ST7789V (bus reversed)"
                           : r == IdResult::Unexpected     ? "answers (unknown ID)"
                                                           : "no reply");
  return r != IdResult::NoReply;
}

static void strapSearch() {
  Serial.println("Strap search:");
  // 1) all floating
  for (auto &l : strapLevels) l = -1;
  if (initAndProbe()) return;

  // 2) every LOW/HIGH combination on the pins the panel does not drive
  uint8_t drivable[EXTRA_COUNT];
  uint8_t n = 0;
  for (uint8_t i = 0; i < EXTRA_COUNT; ++i) if (!extras[i].panelDriven) drivable[n++] = i;

  for (uint32_t combo = 0; combo < (1UL << n); ++combo) {
    for (auto &l : strapLevels) l = -1;
    for (uint8_t b = 0; b < n; ++b) strapLevels[drivable[b]] = (combo >> b) & 1;
    if (initAndProbe()) return;
  }

  Serial.println("No combination got a reply. Check: RD (LCD 7 -> GPIO2), RS, CS, WR,");
  Serial.println("RESET wiring, VCC on LCD 2 and 3, and solder bridges between data pins.");
  for (auto &l : strapLevels) l = -1;
  applyStraps();
  tft.init();
}

// ---------------------------------------------------------------------------
// Patterns
// ---------------------------------------------------------------------------

static void label(const char *text) {
  tft.setTextDatum(BC_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString(text, tft.width() / 2, tft.height() - 4, 2);
  tft.setTextDatum(TL_DATUM);
}

static void solid(uint16_t color, const char *name) {
  tft.fillScreen(color);
  label(name);
}

static void colorBars() {
  static const uint16_t bars[8] = {TFT_WHITE, TFT_YELLOW, TFT_CYAN,  TFT_GREEN,
                                   TFT_MAGENTA, TFT_RED, TFT_BLUE, TFT_BLACK};
  const int16_t w = tft.width(), h = tft.height();
  const int16_t barH = h / 2;
  for (int i = 0; i < 8; ++i) {
    const int16_t x0 = (w * i) / 8, x1 = (w * (i + 1)) / 8;
    tft.fillRect(x0, 0, x1 - x0, barH, bars[i]);
  }
  // 5-6-5 ramps
  const int16_t rampH = (h - barH) / 4;
  for (int16_t x = 0; x < w; ++x) {
    const uint16_t r5 = uint16_t((x * 31) / (w - 1));
    const uint16_t g6 = uint16_t((x * 63) / (w - 1));
    tft.drawFastVLine(x, barH, rampH, uint16_t(r5 << 11));
    tft.drawFastVLine(x, barH + rampH, rampH, uint16_t(g6 << 5));
    tft.drawFastVLine(x, barH + 2 * rampH, rampH, r5);
    tft.drawFastVLine(x, barH + 3 * rampH, h - barH - 3 * rampH,
                      uint16_t((r5 << 11) | (g6 << 5) | r5));
  }
}

static void alignmentGrid() {
  const int16_t w = tft.width(), h = tft.height();
  tft.fillScreen(TFT_BLACK);
  const uint16_t grid = tft.color565(70, 70, 70);
  for (int16_t x = 0; x < w; x += 20) tft.drawFastVLine(x, 0, h, grid);
  for (int16_t y = 0; y < h; y += 20) tft.drawFastHLine(0, y, w, grid);
  tft.drawRect(0, 0, w, h, TFT_WHITE);
  tft.fillRect(0, 0, 10, 10, TFT_RED);              // top-left
  tft.fillRect(w - 10, 0, 10, 10, TFT_GREEN);       // top-right
  tft.fillRect(0, h - 10, 10, 10, TFT_BLUE);        // bottom-left
  tft.fillRect(w - 10, h - 10, 10, 10, TFT_YELLOW); // bottom-right
  tft.drawFastHLine(w / 2 - 15, h / 2, 31, TFT_WHITE);
  tft.drawFastVLine(w / 2, h / 2 - 15, 31, TFT_WHITE);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString("ST7789V", w / 2, h / 2 - 40, 4);
  tft.drawString(String(w) + " x " + String(h), w / 2, h / 2 + 40, 4);
  tft.setTextDatum(TL_DATUM);
}

static void textPage() {
  tft.fillScreen(TFT_NAVY);
  tft.setTextColor(TFT_WHITE, TFT_NAVY);
  tft.drawString("F0-28LS124-01", 10, 10, 4);
  tft.setTextColor(TFT_YELLOW, TFT_NAVY);
  tft.drawString("ESP32 8-bit 8080 bus", 10, 45, 2);
  tft.setTextColor(TFT_CYAN, TFT_NAVY);
  tft.drawString("Rotation: " + String(rotation), 10, 70, 2);
  tft.drawString(String("Inversion: ") + (inverted ? "ON" : "OFF"), 10, 90, 2);
  tft.setTextColor(TFT_GREEN, TFT_NAVY);
  tft.drawString(String(millis() / 1000), 10, 120, 7);
}

static void speedTest() {
  const uint32_t t0 = micros();
  for (int i = 0; i < 10; ++i) tft.fillScreen(i & 1 ? TFT_RED : TFT_BLUE);
  const uint32_t us = micros() - t0;
  const float fps = 10.0f * 1e6f / float(us);
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextDatum(MC_DATUM);
  tft.drawString("Full-screen fill", tft.width() / 2, tft.height() / 2 - 30, 4);
  tft.drawString(String(fps, 1) + " fps", tft.width() / 2, tft.height() / 2 + 10, 4);
  tft.setTextDatum(TL_DATUM);
  Serial.printf("speed: %.1f full-screen fills per second\n", fps);
}

static void showPattern(uint8_t index) {
  switch (index) {
    case 0: solid(TFT_RED, "RED"); break;
    case 1: solid(TFT_GREEN, "GREEN"); break;
    case 2: solid(TFT_BLUE, "BLUE"); break;
    case 3: solid(TFT_WHITE, "WHITE"); break;
    case 4: solid(TFT_BLACK, "BLACK"); break;
    case 5: colorBars(); break;
    case 6: alignmentGrid(); break;
    case 7: textPage(); break;
    case 8: speedTest(); break;
  }
  Serial.printf("pattern %u\n", index);
}

static constexpr uint8_t PATTERN_COUNT = 9;

static bool glassTest = false;

static void glassStep() {
  static uint8_t phase = 0;
  switch (phase) {
    case 0: tft.writecommand(0x28); Serial.println("glass: DISPLAY OFF"); break;
    case 1: tft.writecommand(0x29); tft.fillScreen(TFT_BLACK); Serial.println("glass: ON + BLACK"); break;
    case 2: tft.fillScreen(TFT_RED); Serial.println("glass: ON + RED"); break;
    case 3: tft.invertDisplay(true); Serial.println("glass: RED + INVERSION ON"); break;
    case 4: tft.invertDisplay(false); tft.writecommand(0x10); Serial.println("glass: SLEEP IN"); break;
    case 5: tft.writecommand(0x11); delay(120); tft.fillScreen(TFT_BLACK); Serial.println("glass: SLEEP OUT + BLACK"); break;
  }
  phase = (phase + 1) % 6;
}

static void gramTest() {
  static const struct { uint16_t color; const char *name; } fills[] = {
      {TFT_RED, "RED"}, {TFT_GREEN, "GREEN"}, {TFT_BLUE, "BLUE"}, {TFT_BLACK, "BLACK"}, {TFT_WHITE, "WHITE"}};
  const int16_t pts[][2] = {{0, 0}, {119, 159}, {239, 319}, {10, 300}};
  bool allOk = true;
  for (auto &f : fills) {
    tft.fillScreen(f.color);
    Serial.printf("  wrote %-5s 0x%04X -> read", f.name, f.color);
    for (auto &p : pts) {
      const uint16_t v = tft.readPixel(p[0], p[1]);
      Serial.printf(" 0x%04X", v);
      // readback of 18-bit GRAM loses LSBs on some controllers; compare top bits
      if ((v & 0xE71C) != (f.color & 0xE71C)) allOk = false;
    }
    Serial.println();
  }
  Serial.println(allOk ? "GRAM OK: pixels reach display memory" : "GRAM MISMATCH (readPixel)");

  // Raw RAMRD: window stays at (0,0) after setAddrWindow; read bytes directly.
  for (auto &f : fills) {
    tft.fillScreen(f.color);
    tft.startWrite();
    tft.setAddrWindow(0, 0, 4, 1);
    tft.endWrite();
    Serial.printf("  raw RAMRD after %-5s:", f.name);
    for (uint8_t i = 1; i <= 7; ++i) Serial.printf(" %02X", tft.readcommand8(0x2E, i));
    Serial.println();
  }
}

// ---------------------------------------------------------------------------

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\n=== F0-28LS124-01 ST7789V parallel test (ESP32) ===");

  delay(100);  // let the panel supply settle before reset
  tft.init();  // hardware reset on TFT_RST, then ST7789 init sequence
  classifyExtras();
  strapSearch();
  Serial.print("Using straps: ");
  printStraps();
  Serial.println();
  readControllerId(true);

  tft.setRotation(rotation);
  tft.fillScreen(TFT_BLACK);
  showPattern(patternIndex);
  lastSwitch = millis();
  Serial.println("commands: n=next p=pause i=invert r=rotate d=read ID s=strap search m=GRAM o=glass");
}

void loop() {
  while (Serial.available()) {
    switch (Serial.read()) {
      case 'n':
        patternIndex = (patternIndex + 1) % PATTERN_COUNT;
        showPattern(patternIndex);
        lastSwitch = millis();
        break;
      case 'p':
        paused = !paused;
        Serial.println(paused ? "paused" : "running");
        break;
      case 'i':
        inverted = !inverted;
        tft.invertDisplay(inverted);
        Serial.printf("inversion %s\n", inverted ? "ON" : "OFF");
        break;
      case 'r':
        rotation = (rotation + 1) % 4;
        tft.setRotation(rotation);
        showPattern(patternIndex);
        Serial.printf("rotation %u\n", rotation);
        break;
      case 'd':
        readControllerId(true);
        break;
      case 'o':
        glassTest = !glassTest;
        paused = glassTest;
        Serial.printf("glass test %s\n", glassTest ? "ON" : "OFF");
        lastSwitch = 0;
        break;
      case 'm':
        paused = true;
        gramTest();
        break;
      case 's':
        strapSearch();
        tft.setRotation(rotation);
        showPattern(patternIndex);
        break;
    }
  }

  if (glassTest && millis() - lastSwitch >= 3000) {
    glassStep();
    lastSwitch = millis();
  }

  if (!paused && millis() - lastSwitch >= PATTERN_MS) {
    patternIndex = (patternIndex + 1) % PATTERN_COUNT;
    showPattern(patternIndex);
    lastSwitch = millis();
  }
}
