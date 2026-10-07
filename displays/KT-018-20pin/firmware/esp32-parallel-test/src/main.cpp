// Author: Dibyendu Mondal (https://github.com/dib-00)
/**
 * KT-018 (20-pin, 8-bit 8080 parallel) control-pin scanner + colour test.
 *
 * Measured on the flex: 1 LEDK, 2 LEDA, 3/20 GND, 4/5 power, 6-11 control
 * signals (order unknown), 12-19 data bus.
 *
 * The scanner tries every assignment of {CS, RS, WR, RD, RESET, spare} to
 * LCD pins 6-11, reads RDDID (0x04) for each, and keeps the one that answers.
 * It then initialises the panel with that assignment and cycles colours.
 *
 * Serial commands (115200): s = rescan, i = toggle inversion, p = pause.
 */

#include <Arduino.h>
#include <algorithm>

// LCD pin -> GPIO
// LCD 6 is a panel output (TE/FMARK) on input-only GPIO34 and is never driven.
static const uint8_t CTRL_GPIO[6] = {34, 15, 4, 2, 18, 23};   // LCD 6..11
static const uint8_t CTRL_LCD[6] = {6, 7, 8, 9, 10, 11};
static const uint8_t DATA_GPIO[8] = {12, 13, 26, 19, 17, 16, 27, 14};  // LCD 12..19

enum Role : uint8_t { CS, RS, WR, RD, RST, SPARE, ROLE_COUNT };
static const char *ROLE_NAME[ROLE_COUNT] = {"CS", "RS", "WR", "RD", "RESET", "spare"};

static int8_t rolePin[ROLE_COUNT];  // role -> GPIO
static int8_t roleLcd[ROLE_COUNT];  // role -> LCD pin
static bool ctrlPanelDriven[6];
static bool busReversed = false;    // LCD12 = D7 instead of D0
static bool found = false;
static bool inverted = false;
static bool paused = false;

// ---------------------------------------------------------------------------
// Low-level 8080 bus
// ---------------------------------------------------------------------------

static inline void pinSet(Role r, int level) {
  if (rolePin[r] >= 0) digitalWrite(rolePin[r], level);
}

static void busOutput() {
  for (uint8_t g : DATA_GPIO) pinMode(g, OUTPUT);
}

static bool pullUpMode = false;

static void busInput() {
  for (uint8_t g : DATA_GPIO) pinMode(g, pullUpMode ? INPUT_PULLUP : INPUT_PULLDOWN);
}

static void busWrite(uint8_t v) {
  for (uint8_t i = 0; i < 8; ++i) {
    const uint8_t bit = busReversed ? (v >> (7 - i)) & 1 : (v >> i) & 1;
    digitalWrite(DATA_GPIO[i], bit);
  }
}

static uint8_t busRead() {
  uint8_t v = 0;
  for (uint8_t i = 0; i < 8; ++i) {
    if (digitalRead(DATA_GPIO[i])) v |= busReversed ? (0x80 >> i) : (1 << i);
  }
  return v;
}

static void strobeWrite(uint8_t v) {
  busWrite(v);
  pinSet(WR, LOW);
  delayMicroseconds(1);
  pinSet(WR, HIGH);
}

static void writeCmd(uint8_t c) {
  pinSet(CS, LOW);
  pinSet(RS, LOW);
  strobeWrite(c);
  pinSet(RS, HIGH);
  pinSet(CS, HIGH);
}

static void writeData(const uint8_t *d, size_t n) {
  pinSet(CS, LOW);
  pinSet(RS, HIGH);
  for (size_t i = 0; i < n; ++i) strobeWrite(d[i]);
  pinSet(CS, HIGH);
}

static void cmdData(uint8_t c, std::initializer_list<uint8_t> d) {
  writeCmd(c);
  if (d.size()) writeData(d.begin(), d.size());
}

// Reads n bytes after a command; out[0] is the 8080 dummy byte.
static void readReg(uint8_t cmd, uint8_t *out, uint8_t n) {
  pinSet(CS, LOW);
  pinSet(RS, LOW);
  strobeWrite(cmd);
  pinSet(RS, HIGH);
  busInput();
  for (uint8_t i = 0; i < n; ++i) {
    pinSet(RD, LOW);
    delayMicroseconds(2);
    out[i] = busRead();
    pinSet(RD, HIGH);
    delayMicroseconds(1);
  }
  pinSet(CS, HIGH);
  busOutput();
}

// Register-index controllers (ILI9225, S6D0144, HX8340...): 16-bit index write
// as two bytes with RS low, then data reads with RS high.
static void readIndexReg(uint16_t index, uint8_t *out, uint8_t n) {
  pinSet(CS, LOW);
  pinSet(RS, LOW);
  strobeWrite(index >> 8);
  strobeWrite(index & 0xFF);
  pinSet(RS, HIGH);
  busInput();
  for (uint8_t i = 0; i < n; ++i) {
    pinSet(RD, LOW);
    delayMicroseconds(2);
    out[i] = busRead();
    pinSet(RD, HIGH);
    delayMicroseconds(1);
  }
  pinSet(CS, HIGH);
  busOutput();
}

// True if the panel actively drives the bus during RD: the value read with
// ESP32 pull-downs equals the value read with pull-ups (a floating bus would
// read 00 vs FF).
static bool panelDrivesBus(bool indexStyle, uint8_t *bytes) {
  uint8_t lo[4], hi[4];
  pullUpMode = false;
  if (indexStyle) readIndexReg(0x0000, lo, 3); else readReg(0x04, lo, 4);
  pullUpMode = true;
  if (indexStyle) readIndexReg(0x0000, hi, 3); else readReg(0x04, hi, 4);
  pullUpMode = false;
  const uint8_t n = indexStyle ? 3 : 4;
  memcpy(bytes, lo, n);
  for (uint8_t i = 1; i < n; ++i) if (lo[i] != hi[i]) return false;
  return true;
}

// ---------------------------------------------------------------------------
// Pin classification & scan
// ---------------------------------------------------------------------------

static bool pinDriven(uint8_t g) {
  pinMode(g, INPUT_PULLUP);   delay(3); const int pu = digitalRead(g);
  pinMode(g, INPUT_PULLDOWN); delay(3); const int pd = digitalRead(g);
  pinMode(g, INPUT);
  return pu == pd;
}

static void classify() {
  Serial.println("Pin classification (pull-up / pull-down):");
  for (uint8_t i = 0; i < 6; ++i) {
    // GPIO34-39 are input-only with no pulls: treat as a known panel output.
    ctrlPanelDriven[i] = CTRL_GPIO[i] >= 34 ? true : pinDriven(CTRL_GPIO[i]);
    Serial.printf("  LCD %-2u GPIO%-2u %s\n", CTRL_LCD[i], CTRL_GPIO[i],
                  ctrlPanelDriven[i] ? "DRIVEN BY PANEL (output)" : "input");
  }
  for (uint8_t i = 0; i < 8; ++i) {
    const bool d = pinDriven(DATA_GPIO[i]);
    Serial.printf("  LCD %-2u GPIO%-2u %s\n", 12 + i, DATA_GPIO[i],
                  d ? "DRIVEN BY PANEL (unexpected for data!)" : "input");
  }
}

static int8_t spareLevel = -1;  // -1 floating, 0 LOW, 1 HIGH

static void applyRoles(const uint8_t *roleOfCtrl) {
  for (uint8_t i = 0; i < 6; ++i) {
    const Role r = Role(roleOfCtrl[i]);
    rolePin[r] = CTRL_GPIO[i];
    roleLcd[r] = CTRL_LCD[i];
  }
  for (uint8_t r = 0; r < ROLE_COUNT; ++r) {
    if (r == SPARE) {
      if (spareLevel < 0 || rolePin[r] >= 34) pinMode(rolePin[r], INPUT);  // 34-39 input-only
      else { pinMode(rolePin[r], OUTPUT); digitalWrite(rolePin[r], spareLevel); }
      continue;
    }
    pinMode(rolePin[r], OUTPUT);
    digitalWrite(rolePin[r], HIGH);  // all control lines idle high
  }
  busOutput();
  busWrite(0);
}

static bool looksLikeId(const uint8_t *b) {
  // b[0] dummy, b[1..3] ID. Reject a bus that just floats or holds a level.
  if (b[1] == b[2] && b[2] == b[3]) return false;
  return true;
}

static const char *knownController(const uint8_t *b) {
  if (b[1] == 0x7C && b[2] == 0x89 && b[3] == 0xF0) return "ST7735S";
  if (b[1] == 0x54 && b[2] == 0x80 && b[3] == 0x66) return "ILI9163C";
  if (b[1] == 0x85 && b[2] == 0x85 && b[3] == 0x52) return "ST7789V";
  if (b[1] == 0x00 && b[2] == 0x91 && b[3] == 0x06) return "GC9106";
  return "unknown";
}

static bool foundIndexStyle = false;

static void scan() {
  found = false;
  classify();
  Serial.println("Scanning 720 orders x 3 spare levels x 2 protocols (about 30 s)...");

  uint8_t perm[6] = {CS, RS, WR, RD, RST, SPARE};
  std::sort(perm, perm + 6);
  uint16_t tried = 0, hits = 0;
  uint8_t best[6];
  int8_t bestSpare = -1;

  do {
    bool skip = false;
    for (uint8_t i = 0; i < 6; ++i) {
      if (ctrlPanelDriven[i] && perm[i] != SPARE) { skip = true; break; }
    }
    if (skip) continue;

    // The spare can only be driven when it sits on an output-capable GPIO.
    uint8_t spareIdx = 0;
    for (uint8_t i = 0; i < 6; ++i) if (perm[i] == SPARE) spareIdx = i;
    const int8_t spMax = CTRL_GPIO[spareIdx] >= 34 ? -1 : 1;
    for (int8_t sp = -1; sp <= spMax; ++sp) {
      ++tried;
      spareLevel = sp;
      busReversed = false;
      applyRoles(perm);
      delay(6);  // recover if a previous wrong order pulsed the real RESET
      for (uint8_t style = 0; style < 2; ++style) {
        uint8_t b[4] = {0};
        if (!panelDrivesBus(style == 1, b)) continue;
        ++hits;
        if (hits <= 40) {
          Serial.printf("  HIT %s spare=%s: ", style ? "INDEX-REG" : "DCS-0x04",
                        sp < 0 ? "Z" : sp ? "1" : "0");
          for (uint8_t i = 0; i < 6; ++i) Serial.printf("LCD%u=%s ", CTRL_LCD[i], ROLE_NAME[perm[i]]);
          Serial.printf(" bytes=%02X %02X %02X %02X\n", b[0], b[1], b[2], style ? 0 : b[3]);
        }
        if (!found) {
          memcpy(best, perm, 6);
          bestSpare = sp;
          foundIndexStyle = style == 1;
          found = true;
        }
      }
    }
  } while (std::next_permutation(perm, perm + 6));

  Serial.printf("Scan done: %u combinations tried, %u hit(s)\n", tried, hits);
  if (found) {
    spareLevel = bestSpare;
    applyRoles(best);
    Serial.print("USING: ");
    for (uint8_t r = 0; r < ROLE_COUNT; ++r) Serial.printf("%s=LCD%d ", ROLE_NAME[r], roleLcd[r]);
    Serial.printf("spare=%s protocol=%s\n", bestSpare < 0 ? "Z" : bestSpare ? "1" : "0",
                  foundIndexStyle ? "index-register" : "DCS");
  } else {
    Serial.println("Panel never drove the bus. Check VCC on LCD 4 and 5 (3.3 V while running),");
    Serial.println("GND on LCD 3 and 20, and solder bridges.");
  }
}

// ---------------------------------------------------------------------------
// Panel init + colour test (128x160, window covers 132x162 controllers too)
// ---------------------------------------------------------------------------

static void panelInit() {
  pinSet(RST, HIGH); delay(5);
  pinSet(RST, LOW);  delay(20);
  pinSet(RST, HIGH); delay(150);
  writeCmd(0x01); delay(150);            // SWRESET
  writeCmd(0x11); delay(150);            // SLPOUT
  cmdData(0x3A, {0x05}); delay(10);      // COLMOD 16 bpp
  cmdData(0x36, {0xC0});                 // MADCTL: MY|MX = 180 deg (flex at top)
  writeCmd(inverted ? 0x21 : 0x20);      // INVON / INVOFF
  writeCmd(0x13); delay(10);             // NORON
  writeCmd(0x29); delay(50);             // DISPON
}

static void fill(uint16_t color) {
  // Exactly 128x160: this controller rejects windows beyond its size.
  cmdData(0x2A, {0, 0, 0, 127});
  cmdData(0x2B, {0, 0, 0, 159});
  writeCmd(0x2C);
  pinSet(CS, LOW);
  pinSet(RS, HIGH);
  const uint8_t hi = color >> 8, lo = color & 0xFF;
  for (uint32_t i = 0; i < 128UL * 160UL; ++i) {
    strobeWrite(hi);
    strobeWrite(lo);
  }
  pinSet(CS, HIGH);
}

// ---------------------------------------------------------------------------
// Verification: write/read-back on the 12 candidates left by the scan
// (CS/RD on LCD 7/11, RS/WR/RESET on LCD 8/9/10, LCD 6 = panel output)
// ---------------------------------------------------------------------------

// perm index = control pin (LCD 6..11) -> role
static bool verifyCandidates() {
  static const uint8_t rsWrRst[6][3] = {{RS, WR, RST}, {RS, RST, WR}, {WR, RS, RST},
                                        {WR, RST, RS}, {RST, RS, WR}, {RST, WR, RS}};
  bool ok = false;
  uint8_t winner[6];
  bool winnerRev = false;
  Serial.println("Verifying 12 candidates (reset, write COLMOD, read it back):");
  for (uint8_t csFirst = 0; csFirst < 2; ++csFirst) {
    for (auto &t : rsWrRst) {
      uint8_t perm[6];
      perm[0] = SPARE;                        // LCD 6
      perm[1] = csFirst ? CS : RD;            // LCD 7
      perm[2] = t[0]; perm[3] = t[1]; perm[4] = t[2];  // LCD 8, 9, 10
      perm[5] = csFirst ? RD : CS;            // LCD 11
      spareLevel = -1;
      busReversed = false;
      applyRoles(perm);
      pinSet(RST, LOW);  delay(20);
      pinSet(RST, HIGH); delay(150);
      writeCmd(0x01); delay(150);             // SWRESET
      writeCmd(0x11); delay(150);             // SLPOUT
      uint8_t c5[2], c6[2], id[4], pm[2];
      cmdData(0x3A, {0x05}); readReg(0x0C, c5, 2);
      cmdData(0x3A, {0x06}); readReg(0x0C, c6, 2);
      readReg(0x04, id, 4);
      writeCmd(0x29); delay(20);
      readReg(0x0A, pm, 2);
      for (uint8_t i = 0; i < 6; ++i) Serial.printf("LCD%u=%-5s ", CTRL_LCD[i], ROLE_NAME[perm[i]]);
      Serial.printf("| COLMOD 05->%02X 06->%02X | ID %02X %02X %02X | PM %02X", c5[1], c6[1], id[1], id[2], id[3], pm[1]);
      const bool match = (c5[1] & 0x77) == 0x05 && (c6[1] & 0x77) == 0x06;
      const bool matchRev = (c5[1] & 0xEE) == 0xA0 && (c6[1] & 0xEE) == 0x60;
      if (match || matchRev) {
        Serial.printf("  <== WORKS%s", matchRev ? " (data bus reversed)" : "");
        if (!ok) { memcpy(winner, perm, 6); winnerRev = matchRev; ok = true; }
      }
      Serial.println();
    }
  }
  if (ok) {
    spareLevel = -1;
    applyRoles(winner);
    busReversed = winnerRev;
    foundIndexStyle = false;
    Serial.print("VERIFIED: ");
    for (uint8_t r = 0; r < ROLE_COUNT; ++r) Serial.printf("%s=LCD%d ", ROLE_NAME[r], roleLcd[r]);
    Serial.printf("bus=%s\n", busReversed ? "reversed" : "normal");
  } else {
    Serial.println("No candidate echoed COLMOD back.");
  }
  return ok;
}

static void fillRect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color) {
  cmdData(0x2A, {uint8_t(x >> 8), uint8_t(x), uint8_t((x + w - 1) >> 8), uint8_t(x + w - 1)});
  cmdData(0x2B, {uint8_t(y >> 8), uint8_t(y), uint8_t((y + h - 1) >> 8), uint8_t(y + h - 1)});
  writeCmd(0x2C);
  pinSet(CS, LOW);
  pinSet(RS, HIGH);
  for (uint32_t i = 0; i < uint32_t(w) * h; ++i) {
    strobeWrite(color >> 8);
    strobeWrite(color & 0xFF);
  }
  pinSet(CS, HIGH);
}

// 7-segment digit, 40x70 px, segment thickness 8
static void drawDigit(uint16_t x, uint16_t y, uint8_t d, uint16_t color) {
  static const uint8_t SEG[10] = {0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F};
  const uint8_t m = SEG[d];
  const uint16_t W = 40, H = 70, T = 8, half = H / 2;
  if (m & 0x01) fillRect(x, y, W, T, color);                        // a
  if (m & 0x02) fillRect(x + W - T, y, T, half, color);             // b
  if (m & 0x04) fillRect(x + W - T, y + half, T, half, color);      // c
  if (m & 0x08) fillRect(x, y + H - T, W, T, color);                // d
  if (m & 0x10) fillRect(x, y + half, T, half, color);              // e
  if (m & 0x20) fillRect(x, y, T, half, color);                     // f
  if (m & 0x40) fillRect(x, y + half - T / 2, W, T, color);         // g
}

static void drawNumber(uint8_t n, uint16_t color) {
  if (n >= 10) {
    drawDigit(14, 45, n / 10, color);
    drawDigit(74, 45, n % 10, color);
  } else {
    drawDigit(44, 45, n, color);
  }
}

// Visual test: candidate N (1..12) initialises the panel, then flashes the
// display off/on N times (white flashes), then tries a red fill. Count the flashes on the candidate that shows colour.
static void blinkCandidates() {
  static const uint8_t rsWrRst[6][3] = {{RS, WR, RST}, {RS, RST, WR}, {WR, RS, RST},
                                        {WR, RST, RS}, {RST, RS, WR}, {RST, WR, RS}};
  uint8_t n = 0;
  for (uint8_t csFirst = 0; csFirst < 2; ++csFirst) {
    for (auto &t : rsWrRst) {
      ++n;
      uint8_t perm[6] = {SPARE, uint8_t(csFirst ? CS : RD), t[0], t[1], t[2], uint8_t(csFirst ? RD : CS)};
      spareLevel = -1;
      busReversed = true;   // normal order showed nothing; try LCD12 = D7
      applyRoles(perm);
      Serial.printf("candidate %2u: ", n);
      for (uint8_t i = 0; i < 6; ++i) Serial.printf("LCD%u=%-5s ", CTRL_LCD[i], ROLE_NAME[perm[i]]);
      Serial.println();
      panelInit();
      // Only the correct arrangement can draw: red screen + its number in white.
      fill(0xF800);
      drawNumber(n, 0xFFFF);
      delay(3500);
      fill(0x0000);   // blank between candidates
      delay(500);
    }
  }
  Serial.println("blink round done, repeating");
}

// Pin order found by the visual test (candidate 11), LCD 6..11:
// 6 = TE output, 7 = CS, 8 = RESET, 9 = RS, 10 = WR, 11 = RD.
// Data bus is reversed: LCD 12 = D7 ... LCD 19 = D0.
static const uint8_t FIXED_ROLES[6] = {SPARE, CS, RST, RS, WR, RD};
static const bool FIXED_BUS_REVERSED = true;
static const bool RUN_SCANNER = false;  // true = rerun the pin search at boot

static void alignmentScreen() {
  fill(0x0000);
  fillRect(0, 0, 128, 1, 0xFFFF);      // 1 px border at 128x160
  fillRect(0, 159, 128, 1, 0xFFFF);
  fillRect(0, 0, 1, 160, 0xFFFF);
  fillRect(127, 0, 1, 160, 0xFFFF);
  fillRect(2, 2, 10, 10, 0xF800);      // top-left red
  fillRect(116, 2, 10, 10, 0x07E0);    // top-right green
  fillRect(2, 148, 10, 10, 0x001F);    // bottom-left blue
  fillRect(116, 148, 10, 10, 0xFFE0);  // bottom-right yellow
  drawDigit(44, 45, 8, 0xFFFF);        // upright "8" in the middle
}

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\n=== KT-018 8-bit parallel test (ESP32) ===");
  if (RUN_SCANNER) {
    scan();
    if (found && !verifyCandidates()) {
      Serial.println("Read-back failed -> visual test: note the number on the red screen.");
      for (;;) blinkCandidates();
    }
  } else {
    spareLevel = -1;
    busReversed = FIXED_BUS_REVERSED;
    applyRoles(FIXED_ROLES);
    found = true;
    foundIndexStyle = false;
    Serial.println("Using fixed pinout: CS=LCD7 RESET=LCD8 RS=LCD9 WR=LCD10 RD=LCD11, bus reversed");
  }
  if (found && !foundIndexStyle) panelInit();
}

void loop() {
  static const struct { uint16_t c; const char *n; } colors[] = {
      {0xF800, "RED"}, {0x07E0, "GREEN"}, {0x001F, "BLUE"}, {0xFFFF, "WHITE"}, {0x0000, "BLACK"},
      {0x0000, "ALIGNMENT"}};
  static uint8_t idx = 0;
  static uint32_t last = 0;

  while (Serial.available()) {
    switch (Serial.read()) {
      case 's': scan(); if (found) panelInit(); break;
      case 'i':
        inverted = !inverted;
        writeCmd(inverted ? 0x21 : 0x20);
        Serial.printf("inversion %s\n", inverted ? "ON" : "OFF");
        break;
      case 'p': paused = !paused; Serial.println(paused ? "paused" : "running"); break;
    }
  }

  if (found && !foundIndexStyle && !paused && millis() - last >= 2000) {
    last = millis();
    if (idx == 5) alignmentScreen();
    else fill(colors[idx].c);
    Serial.printf("showing %s\n", colors[idx].n);
    idx = (idx + 1) % 6;
  }
}
