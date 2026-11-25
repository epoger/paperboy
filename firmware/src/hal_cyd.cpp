// Board layer for the "Cheap Yellow Display" ESP32-2432S028R:
// ILI9341 / ST7789 panel via TFT_eSPI, XPT2046 resistive touch on its own SPI bus.
#include <SPI.h>
#include <TFT_eSPI.h>
#include <TJpg_Decoder.h>
#include <XPT2046_Touchscreen.h>

#include "config.h"
#include "hal.h"

static_assert(SCREEN_ROTATION == 0 || SCREEN_ROTATION == 2, "SCREEN_ROTATION must be 0 or 2 (portrait)");

#define TOUCH_CS 33
#define TOUCH_IRQ 36
#define TOUCH_SCLK 25
#define TOUCH_MISO 39
#define TOUCH_MOSI 32
// Raw ADC range of the XPT2046 on a typical CYD. Adjust if taps land off-target.
#define TOUCH_X_MIN 200
#define TOUCH_X_MAX 3700
#define TOUCH_Y_MIN 240
#define TOUCH_Y_MAX 3800

static TFT_eSPI tft;
static XPT2046_Touchscreen ts(TOUCH_CS, TOUCH_IRQ);

static bool jpgOut(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t *bmp) {
  if (y >= tft.height()) return false;
  tft.pushImage(x, y, w, h, bmp);
  return true;
}

namespace hal {

void begin() {
  // the CYD's RGB LED is active-low and very bright: switch it off
  for (int pin : {4, 16, 17}) {
    pinMode(pin, OUTPUT);
    digitalWrite(pin, HIGH);
  }
  tft.init();
  tft.setRotation(SCREEN_ROTATION);
  tft.setSwapBytes(true);  // byte swapping happens once, here; the JPEG decoder must not swap again

  // The display runs on HSPI (see USE_HSPI_PORT in platformio.ini); touch gets the default VSPI bus.
  SPI.begin(TOUCH_SCLK, TOUCH_MISO, TOUCH_MOSI, TOUCH_CS);
  ts.begin();
  ts.setRotation(1);  // raw, native orientation; the portrait mapping is done in touched()

  TJpgDec.setJpgScale(1);
  TJpgDec.setCallback(jpgOut);
}

uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) { return tft.color565(r, g, b); }
void fillScreen(uint16_t c) { tft.fillScreen(c); }
void fillRect(int x, int y, int w, int h, uint16_t c) { tft.fillRect(x, y, w, h, c); }
void fillRoundRect(int x, int y, int w, int h, int r, uint16_t c) { tft.fillRoundRect(x, y, w, h, r, c); }

void setFont(Font f) {
  switch (f) {
    case FONT_SMALL: tft.setTextFont(2); break;
    case FONT_BODY: tft.setFreeFont(&FreeSansBold9pt7b); break;
    case FONT_TITLE: tft.setFreeFont(&FreeSansBold12pt7b); break;
    case FONT_BIG: tft.setFreeFont(&FreeSansBold24pt7b); break;
  }
}

int textWidth(const String &s) { return tft.textWidth(s); }
int lineHeight() { return tft.fontHeight(); }

void drawText(const String &s, int x, int y, Datum d, uint16_t fg, uint16_t /*bg: transparent here*/) {
  static const uint8_t datums[] = {TL_DATUM, TC_DATUM, TR_DATUM, ML_DATUM, MC_DATUM};
  tft.setTextDatum(datums[d]);
  tft.setTextColor(fg);
  tft.drawString(s, x, y);
}

bool drawJpeg(const uint8_t *buf, size_t len, int x, int y) { return TJpgDec.drawJpg(x, y, buf, len) == 0; }

bool touched(int &x, int &y) {
  static bool wasDown = false;
  if (!ts.touched()) {
    wasDown = false;
    return false;
  }
  TS_Point p = ts.getPoint();
  // The touch layer is kept in the panel's native landscape orientation (rotation 1):
  // raw x runs along the 320 px side, raw y along the 240 px side. Turn that into
  // portrait coordinates for the current display rotation.
  int xl = constrain(map(p.x, TOUCH_X_MIN, TOUCH_X_MAX, 0, 319), 0, 319);
  int yl = constrain(map(p.y, TOUCH_Y_MIN, TOUCH_Y_MAX, 0, 239), 0, 239);
  x = (SCREEN_ROTATION == 0) ? (board::W - 1 - yl) : yl;
  y = (SCREEN_ROTATION == 0) ? xl : (board::H - 1 - xl);
  if (!wasDown) Serial.printf("touch raw(%d,%d) -> (%d,%d)\n", p.x, p.y, x, y);
  wasDown = true;
  return true;
}

}  // namespace hal
