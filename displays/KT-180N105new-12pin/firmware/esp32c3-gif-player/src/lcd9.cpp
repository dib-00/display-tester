// Author: Dibyendu Mondal (https://github.com/dib-00)
#include "lcd9.h"

#include <esp_heap_caps.h>

bool Lcd9::begin() {
  pinMode(cs_, OUTPUT);
  digitalWrite(cs_, HIGH);
  pinMode(rst_, OUTPUT);
  digitalWrite(rst_, HIGH);

  spi_bus_config_t bus;
  memset(&bus, 0, sizeof(bus));
  bus.mosi_io_num = sda_;
  bus.miso_io_num = -1;
  bus.sclk_io_num = sck_;
  bus.quadwp_io_num = -1;
  bus.quadhd_io_num = -1;
  bus.max_transfer_sz = BUF_BYTES;
  if (spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO) != ESP_OK) return false;

  spi_device_interface_config_t dev;
  memset(&dev, 0, sizeof(dev));
  dev.mode = 0;                 // panel samples SDA on the rising SCK edge
  dev.clock_speed_hz = int(hz_);
  dev.spics_io_num = -1;        // CS is held low across many transactions
  dev.queue_size = 1;
  dev.flags = SPI_DEVICE_HALFDUPLEX;
  if (spi_bus_add_device(SPI2_HOST, &dev, &spi_) != ESP_OK) return false;

  buf_ = static_cast<uint8_t *>(heap_caps_malloc(BUF_BYTES, MALLOC_CAP_DMA));
  if (!buf_) return false;

  // Hardware reset, then the init sequence verified on KT-180N105new-12pin.
  digitalWrite(rst_, LOW);
  delay(20);
  digitalWrite(rst_, HIGH);
  delay(150);

  select(); cmd(0x01); deselect(); delay(150);   // SWRESET
  select(); cmd(0x11); deselect(); delay(255);   // SLPOUT
  select();
  cmd(0x3A); dat(0x05);                          // COLMOD: RGB565
  cmd(0x36); dat(0x08);                          // MADCTL: BGR colour order
  cmd(0x20);                                     // INVOFF
  cmd(0x13);                                     // NORON
  deselect();
  delay(10);
  select(); cmd(0x29); deselect(); delay(20);    // DISPON
  return true;
}

void Lcd9::flush() {
  if (!len_) return;
  spi_transaction_t t;
  memset(&t, 0, sizeof(t));
  t.length = len_ * 8;
  t.tx_buffer = buf_;
  spi_device_polling_transmit(spi_, &t);
  len_ = 0;
}

void Lcd9::deselect() {
  if (nb_) {
    // Pad the last partial byte with zeros; the extra clocks form an
    // incomplete word that the panel drops when CS goes high.
    buf_[len_++] = uint8_t(acc_ << (8 - nb_));
    nb_ = 0;
  }
  flush();
  acc_ = 0;
  digitalWrite(cs_, HIGH);
}

void Lcd9::window(uint8_t x0, uint8_t y0, uint8_t x1, uint8_t y1) {
  cmd(0x2A); dat(0); dat(x0); dat(0); dat(x1);   // CASET
  cmd(0x2B); dat(0); dat(y0); dat(0); dat(y1);   // RASET
  cmd(0x2C);                                     // RAMWR
}

void Lcd9::pushFrame(const uint8_t *rgb565be) {
  select();
  window(0, 0, WIDTH_PX - 1, HEIGHT_PX - 1);
  for (size_t i = 0; i < FRAME_BYTES; ++i) dat(rgb565be[i]);
  deselect();
}

void Lcd9::drawPixel(int16_t x, int16_t y, uint16_t color) {
  if (x < 0 || y < 0 || x >= WIDTH_PX || y >= HEIGHT_PX) return;
  select();
  window(uint8_t(x), uint8_t(y), uint8_t(x), uint8_t(y));
  dat(color >> 8);
  dat(color & 0xFF);
  deselect();
}

void Lcd9::fillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color) {
  if (x < 0) { w += x; x = 0; }
  if (y < 0) { h += y; y = 0; }
  if (x + w > WIDTH_PX) w = WIDTH_PX - x;
  if (y + h > HEIGHT_PX) h = HEIGHT_PX - y;
  if (w <= 0 || h <= 0) return;
  select();
  window(uint8_t(x), uint8_t(y), uint8_t(x + w - 1), uint8_t(y + h - 1));
  const uint8_t hi = color >> 8, lo = color & 0xFF;
  for (uint32_t n = uint32_t(w) * h; n; --n) {
    dat(hi);
    dat(lo);
  }
  deselect();
}
