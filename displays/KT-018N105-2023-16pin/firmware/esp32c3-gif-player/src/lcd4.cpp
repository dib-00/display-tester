// Author: Dibyendu Mondal (https://github.com/dib-00)
#include "lcd4.h"

#include <esp_heap_caps.h>

bool Lcd4::begin() {
  pinMode(cs_, OUTPUT);
  digitalWrite(cs_, HIGH);
  pinMode(dc_, OUTPUT);
  digitalWrite(dc_, HIGH);
  pinMode(rst_, OUTPUT);
  digitalWrite(rst_, HIGH);

  spi_bus_config_t bus;
  memset(&bus, 0, sizeof(bus));
  bus.mosi_io_num = mosi_;
  bus.miso_io_num = -1;
  bus.sclk_io_num = sck_;
  bus.quadwp_io_num = -1;
  bus.quadhd_io_num = -1;
  bus.max_transfer_sz = BUF_BYTES;
  if (spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO) != ESP_OK) return false;

  spi_device_interface_config_t dev;
  memset(&dev, 0, sizeof(dev));
  dev.mode = 0;
  dev.clock_speed_hz = int(hz_);
  dev.spics_io_num = -1;  // CS driven by hand
  dev.queue_size = 1;
  dev.flags = SPI_DEVICE_HALFDUPLEX;
  if (spi_bus_add_device(SPI2_HOST, &dev, &spi_) != ESP_OK) return false;

  buf_ = static_cast<uint8_t *>(heap_caps_malloc(BUF_BYTES, MALLOC_CAP_DMA));
  if (!buf_) return false;

  // Hardware reset, then the full ST7735R-style init verified on this panel.
  digitalWrite(rst_, HIGH);
  delay(5);
  digitalWrite(rst_, LOW);
  delay(20);
  digitalWrite(rst_, HIGH);
  delay(150);

  digitalWrite(cs_, LOW);
  command(0x01); delay(150);                               // SWRESET
  command(0x11); delay(500);                               // SLPOUT
  command(0xB1, {0x01, 0x2C, 0x2D});                       // FRMCTR1
  command(0xB2, {0x01, 0x2C, 0x2D});                       // FRMCTR2
  command(0xB3, {0x01, 0x2C, 0x2D, 0x01, 0x2C, 0x2D});     // FRMCTR3
  command(0xB4, {0x07});                                   // INVCTR
  command(0xC0, {0xA2, 0x02, 0x84});                       // PWCTR1
  command(0xC1, {0xC5});                                   // PWCTR2
  command(0xC2, {0x0A, 0x00});                             // PWCTR3
  command(0xC3, {0x8A, 0x2A});                             // PWCTR4
  command(0xC4, {0x8A, 0xEE});                             // PWCTR5
  command(0xC5, {0x0E});                                   // VMCTR1
  command(0x20);                                           // INVOFF
  command(0x3A, {0x05});                                   // COLMOD RGB565
  command(0xE0, {0x02, 0x1C, 0x07, 0x12, 0x37, 0x32, 0x29, 0x2D,
                 0x29, 0x25, 0x2B, 0x39, 0x00, 0x01, 0x03, 0x10});   // GMCTRP1
  command(0xE1, {0x03, 0x1D, 0x07, 0x06, 0x2E, 0x2C, 0x29, 0x2D,
                 0x2E, 0x2E, 0x37, 0x3F, 0x00, 0x00, 0x02, 0x10});   // GMCTRN1
  command(0x13); delay(10);                                // NORON
  command(0x29); delay(100);                               // DISPON
  command(0x36, {0xC0});                                   // MADCTL: 180 deg (flex at bottom)
  digitalWrite(cs_, HIGH);
  return true;
}

void Lcd4::transmit(const uint8_t *data, size_t len) {
  while (len) {
    const size_t n = len < BUF_BYTES ? len : BUF_BYTES;
    if (data != buf_) memcpy(buf_, data, n);  // source may not be DMA-capable
    spi_transaction_t t;
    memset(&t, 0, sizeof(t));
    t.length = n * 8;
    t.tx_buffer = buf_;
    spi_device_polling_transmit(spi_, &t);
    if (data != buf_) data += n;
    len -= n;
  }
}

void Lcd4::command(uint8_t c) {
  digitalWrite(dc_, LOW);
  transmit(&c, 1);
  digitalWrite(dc_, HIGH);
}

void Lcd4::params(const uint8_t *d, size_t n) {
  if (!n) return;
  digitalWrite(dc_, HIGH);
  transmit(d, n);
}

void Lcd4::window(uint8_t x0, uint8_t y0, uint8_t x1, uint8_t y1) {
  const uint8_t ca[4] = {0, x0, 0, x1};
  const uint8_t ra[4] = {0, y0, 0, y1};
  command(0x2A);
  params(ca, 4);
  command(0x2B);
  params(ra, 4);
  command(0x2C);  // RAMWR; DC is left high for the pixel data
}

void Lcd4::pushFrame(const uint8_t *rgb565be) {
  digitalWrite(cs_, LOW);
  window(0, 0, WIDTH_PX - 1, HEIGHT_PX - 1);
  transmit(rgb565be, FRAME_BYTES);
  digitalWrite(cs_, HIGH);
}

void Lcd4::drawPixel(int16_t x, int16_t y, uint16_t color) {
  if (x < 0 || y < 0 || x >= WIDTH_PX || y >= HEIGHT_PX) return;
  const uint8_t px[2] = {uint8_t(color >> 8), uint8_t(color & 0xFF)};
  digitalWrite(cs_, LOW);
  window(uint8_t(x), uint8_t(y), uint8_t(x), uint8_t(y));
  transmit(px, 2);
  digitalWrite(cs_, HIGH);
}

void Lcd4::fillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color) {
  if (x < 0) { w += x; x = 0; }
  if (y < 0) { h += y; y = 0; }
  if (x + w > WIDTH_PX) w = WIDTH_PX - x;
  if (y + h > HEIGHT_PX) h = HEIGHT_PX - y;
  if (w <= 0 || h <= 0) return;

  digitalWrite(cs_, LOW);
  window(uint8_t(x), uint8_t(y), uint8_t(x + w - 1), uint8_t(y + h - 1));
  // Fill the DMA buffer with the colour once, then send it repeatedly.
  size_t total = size_t(w) * h * 2;
  const size_t chunk = total < BUF_BYTES ? total : BUF_BYTES;
  for (size_t i = 0; i < chunk; i += 2) {
    buf_[i] = uint8_t(color >> 8);
    buf_[i + 1] = uint8_t(color & 0xFF);
  }
  while (total) {
    const size_t n = total < BUF_BYTES ? total : BUF_BYTES;
    transmit(buf_, n);
    total -= n;
  }
  digitalWrite(cs_, HIGH);
}
