// Author: Dibyendu Mondal (https://github.com/dib-00)
/**
 * SPI panel pin scanner — finds which of 6 unknown signal pins are
 * SCK / SDA / CS / RESET / DC on a serial (SPI) TFT, by bit-banging an ID read
 * (RDDID 0x04) for every assignment, in both 4-wire (DC pin) and 3-wire
 * (9-bit, no DC) modes. For write-only panels it also has a visual mode that
 * draws each candidate's number on the screen.
 *
 * Detection does not depend on knowing the controller: a hit means the panel
 * actively drove SDA during the read (same bits with the ESP32's pull-up and
 * pull-down) and returned a non-constant pattern twice in a row.
 *
 * Wiring for KT-018N105-2023 (16-pin): LCD signal pins 2, 6, 7, 8, 9, 10 go to
 * the GPIOs below. Serial commands (115200):
 *   s = read-back scan, v = visual scan, r<a> <b> <ms> = replay candidates,
 *   k<n> <madctl> <spare> = confirm one candidate, V = targeted variants,
 *   F<madctl hex> = verified KT-018N105-2023 pinout test, c = colour test.
 */

#include <Arduino.h>
#include <algorithm>
#include <soc/gpio_reg.h>

#if defined(CONFIG_IDF_TARGET_ESP32C3)
// ESP32-C3 SuperMini: avoids GPIO2 (faulty on the test board), 8 (LED), 9 (BOOT)
static const uint8_t SIG_GPIO[6] = {4, 5, 6, 7, 10, 3};
#else
static const uint8_t SIG_GPIO[6] = {18, 19, 21, 22, 23, 25};
#endif
static const uint8_t SIG_LCD[6] = {2, 6, 7, 8, 9, 10};

enum Role : uint8_t { R_SCK, R_SDA, R_CS, R_RST, R_DC, R_SPARE, R_SPARE2 };
static const char *ROLE_NAME[] = {"SCK", "SDA", "CS", "RESET", "DC", "-", "-"};

static int8_t pinOf[7];
static bool threeWire = false;
static int8_t sdoGpio = -1;  // separate data-out pin, -1 = read back on SDA
static bool panelDriven[6];

// ---------------------------------------------------------------------------
// Bit-banged SPI, mode 0 (panel samples SDA on the rising edge)
// ---------------------------------------------------------------------------

// Direct set/clear register writes (~10x faster than digitalWrite). All pins < 32.
static inline void wr(Role r, int v) {
  const int8_t p = pinOf[r];
  if (p < 0) return;
  REG_WRITE(v ? GPIO_OUT_W1TS_REG : GPIO_OUT_W1TC_REG, 1UL << p);
}

static inline void tick() { __asm__ __volatile__("nop; nop; nop; nop; nop; nop; nop; nop;"); }

static void clockBit(int bit) {
  wr(R_SDA, bit);
  tick();
  wr(R_SCK, HIGH);
  tick();
  wr(R_SCK, LOW);
}

static void sendByte(uint8_t b, bool isData) {
  if (threeWire) clockBit(isData ? 1 : 0);
  else wr(R_DC, isData ? HIGH : LOW);
  for (int i = 7; i >= 0; --i) clockBit((b >> i) & 1);
}

static void cmd(uint8_t c) { wr(R_CS, LOW); sendByte(c, false); wr(R_CS, HIGH); }

static void cmdData(uint8_t c, std::initializer_list<uint8_t> d) {
  wr(R_CS, LOW);
  sendByte(c, false);
  for (uint8_t v : d) sendByte(v, true);
  wr(R_CS, HIGH);
}

// Sends a read command, then clocks `bits` bits in on SDA with the given pull.
static uint32_t readBits(uint8_t command, int bits, bool pullUp) {
  wr(R_CS, LOW);
  sendByte(command, false);
  if (!threeWire) wr(R_DC, HIGH);
  const int8_t rx = sdoGpio >= 0 ? sdoGpio : pinOf[R_SDA];
  pinMode(rx, pullUp ? INPUT_PULLUP : INPUT_PULLDOWN);
  delayMicroseconds(3);
  uint32_t v = 0;
  for (int i = 0; i < bits; ++i) {
    wr(R_SCK, HIGH);
    delayMicroseconds(2);
    v = (v << 1) | (digitalRead(rx) ? 1 : 0);
    wr(R_SCK, LOW);
    delayMicroseconds(2);
  }
  wr(R_CS, HIGH);
  if (sdoGpio >= 0) {
    pinMode(sdoGpio, INPUT);
  } else {
    pinMode(pinOf[R_SDA], OUTPUT);
    digitalWrite(pinOf[R_SDA], LOW);
  }
  return v;
}

// ---------------------------------------------------------------------------
// Scan
// ---------------------------------------------------------------------------

static void classify() {
  Serial.println("Pin classification (pull-up / pull-down):");
  for (uint8_t i = 0; i < 6; ++i) {
    const uint8_t g = SIG_GPIO[i];
    pinMode(g, INPUT_PULLUP);   delay(3); const int pu = digitalRead(g);
    pinMode(g, INPUT_PULLDOWN); delay(3); const int pd = digitalRead(g);
    pinMode(g, INPUT);
    panelDriven[i] = (pu == pd);
    Serial.printf("  LCD %-2u GPIO%-2u %s\n", SIG_LCD[i], g,
                  panelDriven[i] ? (pu ? "DRIVEN HIGH by panel" : "DRIVEN LOW by panel") : "input");
  }
}

static void apply(const uint8_t *roleOfPin) {
  for (auto &p : pinOf) p = -1;
  for (uint8_t i = 0; i < 6; ++i) pinOf[roleOfPin[i]] = SIG_GPIO[i];
  for (uint8_t i = 0; i < 6; ++i) {
    const Role r = Role(roleOfPin[i]);
    if (r == R_SPARE || r == R_SPARE2) { pinMode(SIG_GPIO[i], INPUT); continue; }
    pinMode(SIG_GPIO[i], OUTPUT);
    digitalWrite(SIG_GPIO[i], (r == R_SCK || r == R_SDA) ? LOW : HIGH);
  }
}

static bool usesPanelOutput(const uint8_t *roleOfPin) {
  for (uint8_t i = 0; i < 6; ++i)
    if (panelDriven[i] && roleOfPin[i] != R_SPARE && roleOfPin[i] != R_SPARE2) return true;
  return false;
}

static const char *idName(uint32_t id) {
  switch (id) {
    case 0x7C89F0: return "ST7735S";
    case 0x5C89F0: return "ST7735";
    case 0x548066: return "ILI9163";
    case 0x009106: return "GC9106";
    case 0x858552: return "ST7789V";
    default:       return "unknown";
  }
}

static uint8_t best[6];
static bool bestThreeWire = false;
static bool found = false;

static int8_t bestSdo = -1;

static void testOnce(const uint8_t *perm, bool three);

// Try read-back on SDA, then on every spare pin the panel drives (a separate SDO).
static void testPerm(const uint8_t *perm, bool three) {
  sdoGpio = -1;
  testOnce(perm, three);
  for (uint8_t i = 0; i < 6; ++i) {
    if (panelDriven[i] && (perm[i] == R_SPARE || perm[i] == R_SPARE2)) {
      sdoGpio = SIG_GPIO[i];
      testOnce(perm, three);
    }
  }
  sdoGpio = -1;
}

static void testOnce(const uint8_t *perm, bool three) {
  threeWire = three;
  apply(perm);
  delay(8);  // let the panel recover if a wrong order held the real RESET low
  const uint32_t a = readBits(0x04, 25, false) & 0x1FFFFFF;
  const uint32_t b = readBits(0x04, 25, true) & 0x1FFFFFF;
  const uint32_t c = readBits(0x04, 25, false) & 0x1FFFFFF;
  if (a != b || a != c) return;                 // not driven by the panel / unstable
  if (a == 0 || a == 0x1FFFFFF) return;         // constant level
  const uint32_t id = a & 0xFFFFFF;             // drop the dummy bit
  Serial.printf("  HIT %s, read on %s: ", three ? "3-wire" : "4-wire",
                sdoGpio >= 0 ? "SDO" : "SDA");
  for (uint8_t i = 0; i < 6; ++i) Serial.printf("LCD%u=%s ", SIG_LCD[i], ROLE_NAME[perm[i]]);
  Serial.printf(" ID=%06lX (%s)\n", (unsigned long)id, idName(id));
  if (!found || strcmp(idName(id), "unknown") != 0) {
    memcpy(best, perm, 6);
    bestThreeWire = three;
    bestSdo = sdoGpio;
    found = true;
  }
}

static void scan() {
  found = false;
  classify();
  Serial.println("4-wire scan (SCK, SDA, CS, RESET, DC + 1 spare): 720 orders...");
  uint8_t p4[6] = {R_SCK, R_SDA, R_CS, R_RST, R_DC, R_SPARE};
  std::sort(p4, p4 + 6);
  do { if (!usesPanelOutput(p4)) testPerm(p4, false); } while (std::next_permutation(p4, p4 + 6));

  Serial.println("3-wire scan (SCK, SDA, CS, RESET + 2 spare): 360 orders...");
  uint8_t p3[6] = {R_SCK, R_SDA, R_CS, R_RST, R_SPARE, R_SPARE2};
  std::sort(p3, p3 + 6);
  do {
    // R_SPARE/R_SPARE2 are interchangeable: only test one ordering of them.
    uint8_t a = 0, b = 0;
    for (uint8_t i = 0; i < 6; ++i) { if (p3[i] == R_SPARE) a = i; if (p3[i] == R_SPARE2) b = i; }
    if (a > b) continue;
    if (!usesPanelOutput(p3)) testPerm(p3, true);
  } while (std::next_permutation(p3, p3 + 6));

  if (!found) {
    Serial.println("No order answered. Check power on LCD 3/5 (and 12), GND, and solder bridges.");
    return;
  }
  threeWire = bestThreeWire;
  apply(best);
  sdoGpio = bestSdo;
  Serial.print("USING: ");
  for (uint8_t i = 0; i < 6; ++i) Serial.printf("LCD%u=%s ", SIG_LCD[i], ROLE_NAME[best[i]]);
  Serial.printf(" (%s)\n", threeWire ? "3-wire 9-bit" : "4-wire with DC");
  Serial.printf("read-back on %s\n", sdoGpio >= 0 ? "separate SDO pin" : "SDA");
}

// ---------------------------------------------------------------------------
// Colour test with the found pinout (bit-banged, slow but certain)
// ---------------------------------------------------------------------------

static void panelInit() {
  wr(R_RST, LOW);  delay(20);
  wr(R_RST, HIGH); delay(150);
  cmd(0x01); delay(150);          // SWRESET
  cmd(0x11); delay(255);          // SLPOUT
  cmdData(0x3A, {0x05});          // RGB565
  cmdData(0x36, {0x00});          // MADCTL
  cmd(0x20);                      // INVOFF
  cmd(0x13);
  cmd(0x29); delay(20);           // DISPON
  const uint32_t pm = readBits(0x0A, 8, false);
  Serial.printf("init done, RDDPM=%02lX (9C = display on)\n", (unsigned long)pm);
}

static void fill(uint16_t color) {
  cmdData(0x2A, {0, 0, 0, 127});
  cmdData(0x2B, {0, 0, 0, 159});
  wr(R_CS, LOW);
  sendByte(0x2C, false);
  for (uint32_t i = 0; i < 128UL * 160UL; ++i) {
    sendByte(color >> 8, true);
    sendByte(color & 0xFF, true);
  }
  wr(R_CS, HIGH);
}

static void colourTest() {
  if (!found) { Serial.println("scan first"); return; }
  panelInit();
  static const struct { uint16_t c; const char *n; } cols[] = {
      {0xF800, "RED"}, {0x07E0, "GREEN"}, {0x001F, "BLUE"}, {0xFFFF, "WHITE"}, {0x0000, "BLACK"}};
  for (auto &k : cols) {
    Serial.printf("showing %s\n", k.n);
    fill(k.c);
    delay(1500);
  }
}

// ---------------------------------------------------------------------------
// Visual scan: for panels that never answer reads. Each candidate order inits
// the panel and draws a red screen with its candidate number in white.
// ---------------------------------------------------------------------------

static void fillRect(uint8_t x, uint8_t y, uint8_t w, uint8_t h, uint16_t color) {
  cmdData(0x2A, {0, x, 0, uint8_t(x + w - 1)});
  cmdData(0x2B, {0, y, 0, uint8_t(y + h - 1)});
  wr(R_CS, LOW);
  sendByte(0x2C, false);
  for (uint32_t i = uint32_t(w) * h; i; --i) {
    sendByte(color >> 8, true);
    sendByte(color & 0xFF, true);
  }
  wr(R_CS, HIGH);
}

static void drawDigit(uint8_t x, uint8_t y, uint8_t d, uint16_t c) {
  static const uint8_t SEG[10] = {0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F};
  const uint8_t m = SEG[d], W = 30, H = 50, T = 6, half = H / 2;
  if (m & 0x01) fillRect(x, y, W, T, c);
  if (m & 0x02) fillRect(x + W - T, y, T, half, c);
  if (m & 0x04) fillRect(x + W - T, y + half, T, half, c);
  if (m & 0x08) fillRect(x, y + H - T, W, T, c);
  if (m & 0x10) fillRect(x, y + half, T, half, c);
  if (m & 0x20) fillRect(x, y, T, half, c);
  if (m & 0x40) fillRect(x, y + half - T / 2, W, T, c);
}

static void drawNumber(uint16_t n) {
  const uint8_t d[3] = {uint8_t(n / 100), uint8_t(n / 10 % 10), uint8_t(n % 10)};
  for (uint8_t i = 0; i < 3; ++i) drawDigit(12 + i * 38, 55, d[i], 0xFFFF);
}

static void quickInit() {
  wr(R_RST, LOW);  delay(10);
  wr(R_RST, HIGH); delay(120);
  cmd(0x01); delay(120);          // SWRESET
  cmd(0x11); delay(120);          // SLPOUT
  cmdData(0x3A, {0x05});
  cmdData(0x36, {0x00});
  cmd(0x13);
  cmd(0x29); delay(10);
}

static uint32_t holdMs = 900;

static void showCandidate(uint16_t n, const uint8_t *perm, bool three) {
  threeWire = three;
  apply(perm);
  Serial.printf("candidate %3u (%s): ", n, three ? "3-wire" : "4-wire");
  for (uint8_t i = 0; i < 6; ++i) Serial.printf("LCD%u=%s ", SIG_LCD[i], ROLE_NAME[perm[i]]);
  Serial.println();
  quickInit();
  fillRect(0, 0, 128, 160, 0xF800);
  drawNumber(n);
  Serial.printf("  drawn at %lu ms\n", (unsigned long)millis());
  delay(holdMs);
}

static void visualScan() {
  Serial.println("VISUAL SCAN: note the number shown on the red screen.");
  uint16_t n = 0;
  // LCD 2 is a panel output, so it is always the spare.
  uint8_t r4[5] = {R_SCK, R_SDA, R_CS, R_RST, R_DC};
  std::sort(r4, r4 + 5);
  do {
    const uint8_t perm[6] = {R_SPARE, r4[0], r4[1], r4[2], r4[3], r4[4]};
    showCandidate(++n, perm, false);
    if (Serial.available()) return;
  } while (std::next_permutation(r4, r4 + 5));
  uint8_t r3[5] = {R_SCK, R_SDA, R_CS, R_RST, R_SPARE};
  std::sort(r3, r3 + 5);
  do {
    const uint8_t perm[6] = {R_SPARE2, r3[0], r3[1], r3[2], r3[3], r3[4]};
    showCandidate(++n, perm, true);
    if (Serial.available()) return;
  } while (std::next_permutation(r3, r3 + 5));
  Serial.println("visual scan round done");
}

// Candidate N in the same order as visualScan().
static bool candidatePerm(uint16_t n, uint8_t *perm, bool &three) {
  uint16_t k = 0;
  uint8_t r4[5] = {R_SCK, R_SDA, R_CS, R_RST, R_DC};
  std::sort(r4, r4 + 5);
  do {
    if (++k == n) {
      perm[0] = R_SPARE;
      memcpy(perm + 1, r4, 5);
      three = false;
      return true;
    }
  } while (std::next_permutation(r4, r4 + 5));
  uint8_t r3[5] = {R_SCK, R_SDA, R_CS, R_RST, R_SPARE};
  std::sort(r3, r3 + 5);
  do {
    if (++k == n) {
      perm[0] = R_SPARE2;
      memcpy(perm + 1, r3, 5);
      three = true;
      return true;
    }
  } while (std::next_permutation(r3, r3 + 5));
  return false;
}

// Confirm one candidate: colour cycle, then an alignment screen
// (1 px white border, corners: TL red, TR green, BL blue, BR yellow).
static void confirmCandidate(uint16_t n, uint8_t madctl, int spareLevel) {
  uint8_t perm[6];
  bool three = false;
  if (!candidatePerm(n, perm, three)) { Serial.println("no such candidate"); return; }
  threeWire = three;
  apply(perm);
  // Drive the non-panel-output spare pins (possible mode straps) to a fixed level.
  for (uint8_t i = 0; i < 6; ++i) {
    if ((perm[i] == R_SPARE || perm[i] == R_SPARE2) && !panelDriven[i] && spareLevel >= 0) {
      pinMode(SIG_GPIO[i], OUTPUT);
      digitalWrite(SIG_GPIO[i], spareLevel);
      Serial.printf("  spare LCD%u held %s\n", SIG_LCD[i], spareLevel ? "HIGH" : "LOW");
    }
  }
  Serial.printf("confirm candidate %u (%s), MADCTL=%02X: ", n, three ? "3-wire" : "4-wire", madctl);
  for (uint8_t i = 0; i < 6; ++i) Serial.printf("LCD%u=%s ", SIG_LCD[i], ROLE_NAME[perm[i]]);
  Serial.println();
  quickInit();
  cmdData(0x36, {madctl});
  static const struct { uint16_t c; const char *n; } cols[] = {
      {0xF800, "RED"}, {0x07E0, "GREEN"}, {0x001F, "BLUE"}, {0xFFFF, "WHITE"}, {0x0000, "BLACK"}};
  for (auto &k : cols) {
    Serial.printf("  showing %s\n", k.n);
    fillRect(0, 0, 128, 160, k.c);
    delay(1500);
  }
  fillRect(0, 0, 128, 160, 0x0000);
  fillRect(0, 0, 128, 1, 0xFFFF);
  fillRect(0, 159, 128, 1, 0xFFFF);
  fillRect(0, 0, 1, 160, 0xFFFF);
  fillRect(127, 0, 1, 160, 0xFFFF);
  fillRect(2, 2, 12, 12, 0xF800);
  fillRect(114, 2, 12, 12, 0x07E0);
  fillRect(2, 146, 12, 12, 0x001F);
  fillRect(114, 146, 12, 12, 0xFFE0);
  drawNumber(n);
  Serial.println("  showing ALIGNMENT (held)");
}

// Bit-alignment sweep for candidate N: before each attempt k (0..8) send k
// extra clock pulses, then init + red fill + draw k. If the panel's 9-bit
// word counter is never reset by CS/RESET, exactly one k lines the words up.
static void alignSweep(uint16_t n) {
  uint8_t perm[6];
  bool three = false;
  if (!candidatePerm(n, perm, three)) { Serial.println("no such candidate"); return; }
  threeWire = three;
  apply(perm);
  for (uint8_t k = 0; k < 9; ++k) {
    wr(R_SDA, LOW);
    for (uint8_t i = 0; i < k; ++i) { wr(R_SCK, HIGH); tick(); wr(R_SCK, LOW); tick(); }
    quickInit();
    fillRect(0, 0, 128, 160, 0xF800);
    drawNumber(k);
    Serial.printf("align attempt %u (%u extra clocks) drawn\n", k, k);
    delay(2500);
  }
  Serial.println("align sweep done");
}

// Full ST7735R/S initialisation (frame rate, power control, VCOM, gamma), as
// used by the Adafruit / TFT_eSPI ST7735 drivers. Some panels fade to black
// with only the minimal SWRESET/SLPOUT/COLMOD/DISPON sequence.
static void fullInit() {
  wr(R_RST, HIGH); delay(5);
  wr(R_RST, LOW);  delay(20);
  wr(R_RST, HIGH); delay(150);
  cmd(0x01); delay(150);                                   // SWRESET
  cmd(0x11); delay(500);                                   // SLPOUT
  cmdData(0xB1, {0x01, 0x2C, 0x2D});                       // FRMCTR1
  cmdData(0xB2, {0x01, 0x2C, 0x2D});                       // FRMCTR2
  cmdData(0xB3, {0x01, 0x2C, 0x2D, 0x01, 0x2C, 0x2D});     // FRMCTR3
  cmdData(0xB4, {0x07});                                   // INVCTR
  cmdData(0xC0, {0xA2, 0x02, 0x84});                       // PWCTR1
  cmdData(0xC1, {0xC5});                                   // PWCTR2
  cmdData(0xC2, {0x0A, 0x00});                             // PWCTR3
  cmdData(0xC3, {0x8A, 0x2A});                             // PWCTR4
  cmdData(0xC4, {0x8A, 0xEE});                             // PWCTR5
  cmdData(0xC5, {0x0E});                                   // VMCTR1 (VCOM)
  cmd(0x20);                                               // INVOFF
  cmdData(0x36, {0x00});                                   // MADCTL
  cmdData(0x3A, {0x05});                                   // COLMOD 16 bpp
  cmdData(0xE0, {0x02, 0x1C, 0x07, 0x12, 0x37, 0x32, 0x29, 0x2D,
                 0x29, 0x25, 0x2B, 0x39, 0x00, 0x01, 0x03, 0x10});   // GMCTRP1
  cmdData(0xE1, {0x03, 0x1D, 0x07, 0x06, 0x2E, 0x2C, 0x29, 0x2D,
                 0x2E, 0x2E, 0x37, 0x3F, 0x00, 0x00, 0x02, 0x10});   // GMCTRN1
  cmd(0x13); delay(10);                                    // NORON
  cmd(0x29); delay(100);                                   // DISPON
}

// f <candidate> <spare level 0/1/2=float>: full init, red + number, then the
// alignment screen, each held. Shows whether the image stays.
static void fullInitTest(uint16_t n, int spareLevel) {
  uint8_t perm[6];
  bool three = false;
  if (!candidatePerm(n, perm, three)) { Serial.println("no such candidate"); return; }
  threeWire = three;
  apply(perm);
  for (uint8_t i = 0; i < 6; ++i) {
    if ((perm[i] == R_SPARE || perm[i] == R_SPARE2) && !panelDriven[i] && spareLevel >= 0) {
      pinMode(SIG_GPIO[i], OUTPUT);
      digitalWrite(SIG_GPIO[i], spareLevel);
    }
  }
  Serial.printf("full init, candidate %u, spare %s\n", n,
                spareLevel < 0 ? "floating" : spareLevel ? "HIGH" : "LOW");
  fullInit();
  fillRect(0, 0, 128, 160, 0xF800);
  drawNumber(n);
  Serial.println("  red + number drawn (held)");
}

// Targeted variants with RESET=6, SCK=8, SDA=9 fixed (LCD pin numbers) and
// the CS / DC / mode question on LCD 7 and 10. Each variant does the full init
// and draws a red screen with its own number, held 4 s.
struct Variant {
  int8_t cs, dc;          // LCD pin or -1
  bool three;             // 3-wire 9-bit framing
  int8_t fixLcd, fixLvl;  // an extra LCD pin held at a level (-1 = none)
  const char *note;
};

static int8_t gpioForLcd(int8_t lcd) {
  for (uint8_t i = 0; i < 6; ++i) if (SIG_LCD[i] == lcd) return SIG_GPIO[i];
  return -1;
}

static void runVariant(uint8_t id, const Variant &v) {
  for (uint8_t i = 0; i < 6; ++i) pinMode(SIG_GPIO[i], INPUT);
  for (auto &p : pinOf) p = -1;
  pinOf[R_RST] = gpioForLcd(6);
  pinOf[R_SCK] = gpioForLcd(8);
  pinOf[R_SDA] = gpioForLcd(9);
  pinOf[R_CS] = v.cs >= 0 ? gpioForLcd(v.cs) : -1;
  pinOf[R_DC] = v.dc >= 0 ? gpioForLcd(v.dc) : -1;
  threeWire = v.three;
  const Role outs[] = {R_RST, R_SCK, R_SDA, R_CS, R_DC};
  for (Role r : outs) {
    if (pinOf[r] < 0) continue;
    pinMode(pinOf[r], OUTPUT);
    digitalWrite(pinOf[r], (r == R_SCK || r == R_SDA) ? LOW : HIGH);
  }
  if (v.fixLcd >= 0) {
    pinMode(gpioForLcd(v.fixLcd), OUTPUT);
    digitalWrite(gpioForLcd(v.fixLcd), v.fixLvl);
  }
  Serial.printf("variant %u: %s\n", id, v.note);
  fullInit();
  fillRect(0, 0, 128, 160, 0xF800);
  drawNumber(id);
  delay(4000);
}

static void runVariants() {
  static const Variant V[] = {
      {7, 10, false, -1, 0, "4-wire RST6 CS7 SCK8 SDA9 DC10"},
      {10, 7, false, -1, 0, "4-wire RST6 DC7 SCK8 SDA9 CS10"},
      {7, -1, true, 10, 0, "3-wire RST6 CS7 SCK8 SDA9, LCD10 LOW"},
      {7, -1, true, 10, 1, "3-wire RST6 CS7 SCK8 SDA9, LCD10 HIGH"},
      {10, -1, true, 7, 0, "3-wire RST6 CS10 SCK8 SDA9, LCD7 LOW"},
      {10, -1, true, 7, 1, "3-wire RST6 CS10 SCK8 SDA9, LCD7 HIGH"},
      {-1, 7, false, 10, 0, "4-wire RST6 DC7 SCK8 SDA9, CS10 held LOW"},
      {-1, 10, false, 7, 0, "4-wire RST6 CS7 held LOW, SCK8 SDA9 DC10"},
  };
  for (uint8_t i = 0; i < sizeof(V) / sizeof(V[0]); ++i) runVariant(i + 1, V[i]);
  Serial.println("variants done");
}

// Verified pinout for KT-018N105-2023 (4-wire SPI):
// LCD 6 = RESET, 7 = DC, 8 = SCK, 9 = SDA, 10 = CS (LCD 2 = panel output).
// F <madctl hex>: colour cycle, then hold an alignment screen.
static void fixedTest(uint8_t madctl) {
  const Variant v = {10, 7, false, -1, 0, "fixed 4-wire RST6 DC7 SCK8 SDA9 CS10"};
  for (uint8_t i = 0; i < 6; ++i) pinMode(SIG_GPIO[i], INPUT);
  for (auto &p : pinOf) p = -1;
  pinOf[R_RST] = gpioForLcd(6);
  pinOf[R_DC] = gpioForLcd(7);
  pinOf[R_SCK] = gpioForLcd(8);
  pinOf[R_SDA] = gpioForLcd(9);
  pinOf[R_CS] = gpioForLcd(10);
  threeWire = false;
  const Role outs[] = {R_RST, R_SCK, R_SDA, R_CS, R_DC};
  for (Role r : outs) {
    pinMode(pinOf[r], OUTPUT);
    digitalWrite(pinOf[r], (r == R_SCK || r == R_SDA) ? LOW : HIGH);
  }
  Serial.printf("%s, MADCTL=%02X\n", v.note, madctl);
  fullInit();
  cmdData(0x36, {madctl});
  static const struct { uint16_t c; const char *n; } cols[] = {
      {0xF800, "RED"}, {0x07E0, "GREEN"}, {0x001F, "BLUE"}, {0xFFFF, "WHITE"}, {0x0000, "BLACK"}};
  for (auto &k : cols) {
    Serial.printf("  showing %s\n", k.n);
    fillRect(0, 0, 128, 160, k.c);
    delay(1500);
  }
  fillRect(0, 0, 128, 160, 0x0000);
  fillRect(0, 0, 128, 1, 0xFFFF);
  fillRect(0, 159, 128, 1, 0xFFFF);
  fillRect(0, 0, 1, 160, 0xFFFF);
  fillRect(127, 0, 1, 160, 0xFFFF);
  fillRect(2, 2, 12, 12, 0xF800);      // top-left red
  fillRect(114, 2, 12, 12, 0x07E0);    // top-right green
  fillRect(2, 146, 12, 12, 0x001F);    // bottom-left blue
  fillRect(114, 146, 12, 12, 0xFFE0);  // bottom-right yellow
  drawNumber(123);
  Serial.println("  ALIGNMENT held: border, corners R/G/B/Y, number 123");
}

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\n=== SPI panel pin scanner (ESP32) ===");
  scan();
  colourTest();
}

void loop() {
  if (!Serial.available()) { delay(5); return; }
  switch (Serial.read()) {
    case 's': scan(); break;
    case 'v': while (!Serial.available()) visualScan(); break;
    case 'a': alignSweep(uint16_t(Serial.parseInt())); break;
    case 'V': runVariants(); break;
    case 'F': { while (Serial.peek() == ' ') Serial.read();
                char h[3] = {0}; Serial.readBytes(h, 2);
                fixedTest(uint8_t(strtoul(h, nullptr, 16))); break; }
    case 'L': {
      // L <from> <to> <target>: loop {from..to at 900 ms, then target held 3 s}
      const long a = Serial.parseInt(), b = Serial.parseInt(), t = Serial.parseInt();
      while (Serial.available()) Serial.read();
      uint32_t round = 0;
      while (!Serial.available()) {
        Serial.printf("loop round %lu\n", (unsigned long)++round);
        holdMs = 900;
        for (long n = a; n <= b && !Serial.available(); ++n) {
          uint8_t perm[6];
          bool three = false;
          if (candidatePerm(uint16_t(n), perm, three)) showCandidate(uint16_t(n), perm, three);
        }
        uint8_t perm[6];
        bool three = false;
        holdMs = 3000;
        if (!Serial.available() && candidatePerm(uint16_t(t), perm, three))
          showCandidate(uint16_t(t), perm, three);
      }
      holdMs = 900;
      Serial.println("loop stopped");
      break;
    }
    case 'f': { const long n = Serial.parseInt(); const long sp = Serial.parseInt();
                fullInitTest(uint16_t(n), sp == 2 ? -1 : int(sp)); break; }
    case 'r': {
      const long a = Serial.parseInt(), b = Serial.parseInt(), h = Serial.parseInt();
      holdMs = h > 0 ? uint32_t(h) : 2500;
      for (long n = a; n <= b; ++n) {
        uint8_t perm[6];
        bool three = false;
        if (candidatePerm(uint16_t(n), perm, three)) showCandidate(uint16_t(n), perm, three);
      }
      holdMs = 900;
      Serial.println("range done");
      break;
    }
    case 'k': { const long n = Serial.parseInt(); const long m = Serial.parseInt();
                const long sp = Serial.parseInt();  // spare level: 0, 1 (2 = floating)
                confirmCandidate(uint16_t(n), uint8_t(m), sp == 2 ? -1 : int(sp)); break; }
    case 'c': colourTest(); break;
  }
}
