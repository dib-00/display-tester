// Author: Dibyendu Mondal (https://github.com/dib-00)
/**
 * Driver for ST7735S panels wired for 3-wire serial (no DC pin): every byte
 * is a 9-bit word whose first bit is D/C (0 = command, 1 = data).
 *
 * Uses the ESP32-C3 SPI2 peripheral with DMA. The 9-bit words are packed
 * MSB-first into a byte stream and sent as plain bit-length transactions,
 * with CS held low by hand for the whole transfer. A trailing partial word
 * left over by byte padding is discarded by the panel when CS rises.
 */
#pragma once

#include <Arduino.h>
#include <Adafruit_GFX.h>
#include <driver/spi_master.h>

class Lcd9 : public Adafruit_GFX {
 public:
  static constexpr int16_t WIDTH_PX = 128;
  static constexpr int16_t HEIGHT_PX = 160;
  static constexpr size_t FRAME_BYTES = size_t(WIDTH_PX) * HEIGHT_PX * 2;

  Lcd9(int8_t sck, int8_t sda, int8_t cs, int8_t rst, uint32_t hz)
      : Adafruit_GFX(WIDTH_PX, HEIGHT_PX), sck_(sck), sda_(sda), cs_(cs), rst_(rst), hz_(hz) {}

  bool begin();

  // Full frame, RGB565 big-endian, row-major, 128x160.
  void pushFrame(const uint8_t *rgb565be);

  void drawPixel(int16_t x, int16_t y, uint16_t color) override;
  void fillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color) override;
  void drawFastVLine(int16_t x, int16_t y, int16_t h, uint16_t color) override { fillRect(x, y, 1, h, color); }
  void drawFastHLine(int16_t x, int16_t y, int16_t w, uint16_t color) override { fillRect(x, y, w, 1, color); }
  void fillScreen(uint16_t color) override { fillRect(0, 0, WIDTH_PX, HEIGHT_PX, color); }

 private:
  static constexpr size_t BUF_BYTES = 4608;  // 4096 nine-bit words

  inline void put(uint16_t word9) {
    acc_ = (acc_ << 9) | word9;
    nb_ += 9;
    while (nb_ >= 8) {
      nb_ -= 8;
      buf_[len_++] = uint8_t(acc_ >> nb_);
      if (len_ == BUF_BYTES) flush();
    }
  }
  inline void cmd(uint8_t c) { put(c); }
  inline void dat(uint8_t d) { put(uint16_t(0x100 | d)); }

  void select() { digitalWrite(cs_, LOW); }
  void deselect();
  void flush();
  void window(uint8_t x0, uint8_t y0, uint8_t x1, uint8_t y1);

  int8_t sck_, sda_, cs_, rst_;
  uint32_t hz_;
  spi_device_handle_t spi_ = nullptr;
  uint8_t *buf_ = nullptr;
  size_t len_ = 0;
  uint32_t acc_ = 0;
  uint8_t nb_ = 0;
};
