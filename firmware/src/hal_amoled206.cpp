// Board layer for the Waveshare ESP32-S3-Touch-AMOLED-2.06:
// 410x502 AMOLED (CO5300 controller on a QSPI bus, driven by Arduino_GFX)
// and an FT3168 capacitive touch controller on I2C.
//
// Pin numbers and the CO5300 set-up come from Waveshare's own Arduino example
// (github.com/waveshareteam/ESP32-S3-Touch-AMOLED-2.06, 01_HelloWorld + pin_config.h).
#include <Arduino_GFX_Library.h>
#include <TJpg_Decoder.h>
#include <Wire.h>

#include "config.h"
#include "fonts/FreeSans12pt7b.h"
#include "fonts/FreeSansBold12pt7b.h"
#include "fonts/FreeSansBold18pt7b.h"
#include "fonts/FreeSansBold24pt7b.h"
#include "hal.h"

#ifndef AMOLED_BRIGHTNESS
#define AMOLED_BRIGHTNESS 200  // 0-255
#endif

// ---- pins ------------------------------------------------------------------
#define LCD_CS 12
#define LCD_SCLK 11
#define LCD_SDIO0 4
#define LCD_SDIO1 5
#define LCD_SDIO2 6
#define LCD_SDIO3 7
#define LCD_RESET 8
#define IIC_SDA 15
#define IIC_SCL 14
#define TP_RESET 9
#define FT3168_ADDR 0x38

static Arduino_DataBus *bus = new Arduino_ESP32QSPI(LCD_CS, LCD_SCLK, LCD_SDIO0, LCD_SDIO1, LCD_SDIO2, LCD_SDIO3);
// col_offset1 = 22: the 410-pixel-wide panel is centred in the controller's 466-pixel memory
static Arduino_CO5300 *gfx = new Arduino_CO5300(bus, LCD_RESET, 0 /*rotation*/, board::W, board::H, 22, 0, 0, 0);

static const GFXfont *curFont = &FreeSansBold12pt7b;

// ---- text ------------------------------------------------------------------
// Arduino_GFX would draw these Adafruit-style fonts one pixel at a time. Single-pixel writes
// don't show up on this QSPI AMOLED panel (filled rectangles and bitmaps do), so each text run
// is rendered into an off-screen RGB565 buffer over its background colour and pushed to the
// display as one bitmap. The CO5300 wants windows aligned to even coordinates and sizes, so
// the pushed block is rounded outward to even numbers; that is why `bg` must be the colour
// that is really behind the text.
static const int TBUF_H = 72;  // tallest line we render (the 24 pt font needs ~56 rows)
static uint16_t *tbuf = nullptr;
struct FontMetrics {
  int asc = 0, desc = 0;  // largest extent above / below the baseline over every glyph
  bool ready = false;
};
static FontMetrics metrics[4];
static int curFontIdx = 1;

static const FontMetrics &fontMetrics() {
  FontMetrics &m = metrics[curFontIdx];
  if (!m.ready) {
    for (int c = curFont->first; c <= curFont->last; c++) {
      const GFXglyph *g = &curFont->glyph[c - curFont->first];
      if (g->height == 0) continue;
      m.asc = max(m.asc, -(int)g->yOffset);
      m.desc = max(m.desc, (int)g->yOffset + (int)g->height);
    }
    m.ready = true;
  }
  return m;
}

static bool jpgOut(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t *bmp) {
  if (y >= gfx->height()) return false;
  gfx->draw16bitRGBBitmap(x, y, bmp, w, h);  // TJpgDec's native-endian RGB565 is what Arduino_GFX expects
  return true;
}

// ---- FT3168 touch ----------------------------------------------------------
// Register map (from Waveshare's Arduino_DriveBus): 0x02 finger count, 0x03/0x04 X high(4 bits)/low,
// 0x05/0x06 Y high(4 bits)/low. Coordinates are 12-bit and match the 410x502 panel.
static bool readTouch(int &x, int &y) {
  Wire.beginTransmission(FT3168_ADDR);
  Wire.write(0x02);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((uint8_t)FT3168_ADDR, (uint8_t)5) != 5) return false;
  uint8_t n = Wire.read() & 0x0F;
  uint8_t xh = Wire.read(), xl = Wire.read(), yh = Wire.read(), yl = Wire.read();
  if (n == 0) return false;
  x = constrain(((xh & 0x0F) << 8) | xl, 0, board::W - 1);
  y = constrain(((yh & 0x0F) << 8) | yl, 0, board::H - 1);
  return true;
}

namespace hal {

void begin() {
  // With USB-CDC on boot, Serial writes can block when no computer is attached (watch on battery).
  Serial.setTxTimeoutMs(0);

  if (!gfx->begin()) Serial.println("gfx->begin() failed!");
  gfx->setBrightness(AMOLED_BRIGHTNESS);
  gfx->fillScreen(0x0000);

  size_t tbytes = (size_t)board::W * TBUF_H * sizeof(uint16_t);
  tbuf = (uint16_t *)(psramFound() ? ps_malloc(tbytes) : malloc(tbytes));
  if (!tbuf) Serial.println("ERROR: could not allocate the text buffer");

  Wire.begin(IIC_SDA, IIC_SCL);
  Wire.setClock(400000);
  pinMode(TP_RESET, OUTPUT);  // reset pulse, as Arduino_DriveBus does when given a reset pin
  digitalWrite(TP_RESET, HIGH);
  delay(1);
  digitalWrite(TP_RESET, LOW);
  delay(20);
  digitalWrite(TP_RESET, HIGH);
  delay(50);
  Wire.beginTransmission(FT3168_ADDR);
  Wire.write(0xA0);  // chip ID register: 0x03 on an FT3168
  Wire.endTransmission(false);
  if (Wire.requestFrom((uint8_t)FT3168_ADDR, (uint8_t)1) == 1)
    Serial.printf("FT3168 touch controller found (id 0x%02X)\n", Wire.read());
  else
    Serial.println("WARNING: FT3168 touch controller did not answer on I2C");

  TJpgDec.setJpgScale(1);
  TJpgDec.setCallback(jpgOut);
}

uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) { return gfx->color565(r, g, b); }
void fillScreen(uint16_t c) { gfx->fillScreen(c); }
void fillRect(int x, int y, int w, int h, uint16_t c) { gfx->fillRect(x, y, w, h, c); }
void fillRoundRect(int x, int y, int w, int h, int r, uint16_t c) { gfx->fillRoundRect(x, y, w, h, r, c); }

void setFont(Font f) {
  curFontIdx = (int)f;
  switch (f) {
    case FONT_SMALL: curFont = &FreeSans12pt7b; break;
    case FONT_BODY: curFont = &FreeSansBold12pt7b; break;
    case FONT_TITLE: curFont = &FreeSansBold18pt7b; break;
    case FONT_BIG: curFont = &FreeSansBold24pt7b; break;
  }
}

// Width of the text = the sum of the glyph advances (characters outside the font are skipped).
int textWidth(const String &s) {
  int w = 0;
  for (size_t i = 0; i < s.length(); i++) {
    uint8_t c = s[i];
    if (c >= curFont->first && c <= curFont->last) w += curFont->glyph[c - curFont->first].xAdvance;
  }
  return w;
}

int lineHeight() { return curFont->yAdvance; }

void drawText(const String &s, int x, int y, Datum d, uint16_t fg, uint16_t bg) {
  if (!tbuf || !s.length()) return;
  const FontMetrics &m = fontMetrics();
  int w = textWidth(s);
  int cx = x;  // pen start
  if (d == TC || d == MC) cx = x - w / 2;
  if (d == TR) cx = x - w;
  // Adafruit fonts draw from the baseline: TL/TC/TR put the top of the tallest glyph at y,
  // ML/MC centre the line's full ascender..descender span on y.
  int base = (d == ML || d == MC) ? y + (m.asc - m.desc) / 2 : y + m.asc;

  // ink extent of this particular string, so neighbouring lines aren't wiped more than needed
  int minY = 9999, maxY = -9999;
  for (size_t i = 0; i < s.length(); i++) {
    uint8_t c = s[i];
    if (c < curFont->first || c > curFont->last) continue;
    const GFXglyph *g = &curFont->glyph[c - curFont->first];
    if (g->height == 0) continue;
    minY = min(minY, (int)g->yOffset);
    maxY = max(maxY, (int)g->yOffset + (int)g->height - 1);
  }
  if (minY > maxY) return;  // nothing visible (only spaces / unsupported characters)

  // block pushed to the display: rounded outward to even coordinates and even size, clipped
  int rx0 = max(0, (cx - 2) & ~1);
  int rw = (((cx + w + 3) - rx0 + 1) + 1) & ~1;
  if (rx0 + rw > board::W) rw = (board::W - rx0) & ~1;
  int ry0 = max(0, (base + minY - 1) & ~1);
  int rh = (((base + maxY + 1) - ry0 + 1) + 1) & ~1;
  if (ry0 + rh > board::H) rh = (board::H - ry0) & ~1;
  if (rh > TBUF_H) rh = TBUF_H & ~1;
  if (rw <= 0 || rh <= 0) return;

  for (int i = 0; i < rw * rh; i++) tbuf[i] = bg;

  int pen = cx;
  for (size_t i = 0; i < s.length(); i++) {
    uint8_t c = s[i];
    if (c < curFont->first || c > curFont->last) continue;
    const GFXglyph *g = &curFont->glyph[c - curFont->first];
    const uint8_t *bits = curFont->bitmap + g->bitmapOffset;
    uint8_t cur = 0, bit = 0;
    for (int yy = 0; yy < g->height; yy++) {
      int py = base + g->yOffset + yy - ry0;
      for (int xx = 0; xx < g->width; xx++) {
        if (!(bit++ & 7)) cur = *bits++;  // glyph bitmaps are packed 1 bit per pixel, row after row
        if (cur & 0x80) {
          int px = pen + g->xOffset + xx - rx0;
          if (px >= 0 && px < rw && py >= 0 && py < rh) tbuf[py * rw + px] = fg;
        }
        cur <<= 1;
      }
    }
    pen += g->xAdvance;
  }
  gfx->draw16bitRGBBitmap(rx0, ry0, tbuf, rw, rh);
}

bool drawJpeg(const uint8_t *buf, size_t len, int x, int y) { return TJpgDec.drawJpg(x, y, buf, len) == 0; }

bool touched(int &x, int &y) {
  static uint32_t last = 0;
  static bool down = false;
  static int sx = 0, sy = 0;
  if (millis() - last >= 15) {  // poll the I2C chip at ~66 Hz, not as fast as loop() spins
    last = millis();
    bool now = readTouch(sx, sy);
    if (now && !down) Serial.printf("touch (%d,%d)\n", sx, sy);
    down = now;
  }
  x = sx;
  y = sy;
  return down;
}

}  // namespace hal
