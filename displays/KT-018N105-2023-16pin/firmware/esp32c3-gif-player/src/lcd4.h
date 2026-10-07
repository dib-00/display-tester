// Author: Dibyendu Mondal (https://github.com/dib-00)
/**
 * 4-wire SPI driver (SCK, MOSI, CS, DC, RESET) for the KT-018N105-2023 panel
 * (ST7735-compatible, 128x160, write-only, mounted rotated 180 degrees).
 *
 * Uses the ESP32-C3 SPI2 peripheral with DMA. CS and DC are plain GPIOs, so
 * CS stays low across a whole frame and DC switches between command bytes
 * (low) and parameters / pixels (high).
 */
#pragma once

#include <Arduino.h>
#include <Adafruit_GFX.h>
#include <driver/spi_master.h>

class Lcd4 : public Adafruit_GFX {
 public:
  static constexpr int16_t WIDTH_PX = 128;
  static constexpr int16_t HEIGHT_PX = 160;
  static constexpr size_t FRAME_BYTES = size_t(WIDTH_PX) * HEIGHT_PX * 2;

  Lcd4(int8_t sck, int8_t mosi, int8_t cs, int8_t dc, int8_t rst, uint32_t hz)
      : Adafruit_GFX(WIDTH_PX, HEIGHT_PX), sck_(sck), mosi_(mosi), cs_(cs), dc_(dc), rst_(rst), hz_(hz) {}

  bool begin();

  // Full frame, RGB565 big-endian, row-major, 128x160.
  void pushFrame(const uint8_t *rgb565be);

  void drawPixel(int16_t x, int16_t y, uint16_t color) override;
  void fillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color) override;
  void drawFastVLine(int16_t x, int16_t y, int16_t h, uint16_t color) override { fillRect(x, y, 1, h, color); }
  void drawFastHLine(int16_t x, int16_t y, int16_t w, uint16_t color) override { fillRect(x, y, w, 1, color); }
  void fillScreen(uint16_t color) override { fillRect(0, 0, WIDTH_PX, HEIGHT_PX, color); }

 private:
  static constexpr size_t BUF_BYTES = 4096;

  void transmit(const uint8_t *data, size_t len);  // DMA, in BUF_BYTES chunks
  void command(uint8_t c);                          // DC low
  void params(const uint8_t *d, size_t n);          // DC high
  void command(uint8_t c, std::initializer_list<uint8_t> p) {
    command(c);
    params(p.begin(), p.size());
  }
  void window(uint8_t x0, uint8_t y0, uint8_t x1, uint8_t y1);  // ends after RAMWR, DC high

  int8_t sck_, mosi_, cs_, dc_, rst_;
  uint32_t hz_;
  spi_device_handle_t spi_ = nullptr;
  uint8_t *buf_ = nullptr;
};
