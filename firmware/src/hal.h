// Board abstraction: everything main.cpp needs from the display and touch panel.
// One implementation per board (selected by build_src_filter in platformio.ini):
//   hal_cyd.cpp        "Cheap Yellow Display" ESP32-2432S028R   (TFT_eSPI + XPT2046)
//   hal_amoled206.cpp  Waveshare ESP32-S3-Touch-AMOLED-2.06     (Arduino_GFX + FT3168)
// The screen is always portrait.
#pragma once
#include <Arduino.h>

namespace hal {

enum Font { FONT_SMALL, FONT_BODY, FONT_TITLE, FONT_BIG };
enum Datum { TL, TC, TR, ML, MC };  // which point of the text the (x, y) refers to

constexpr uint16_t WHITE = 0xFFFF;
constexpr uint16_t ORANGE = 0xFDA0;

void begin();  // display, touch, backlight, JPEG decoder
uint16_t rgb(uint8_t r, uint8_t g, uint8_t b);
void fillScreen(uint16_t c);
void fillRect(int x, int y, int w, int h, uint16_t c);
void fillRoundRect(int x, int y, int w, int h, int r, uint16_t c);

// Text is drawn in the colour `fg` over a solid background `bg` that callers have already painted.
// The CYD draws it transparently and ignores `bg`; the AMOLED renders each run into an off-screen
// buffer (single-pixel writes are unreliable on its QSPI panel) and so needs to know `bg`.
void setFont(Font f);  // selects the font used by textWidth / lineHeight / drawText
int textWidth(const String &s);
int lineHeight();
void drawText(const String &s, int x, int y, Datum d, uint16_t fg, uint16_t bg);

bool drawJpeg(const uint8_t *buf, size_t len, int x, int y);  // baseline JPEG

// True while a finger is down; x/y are portrait pixels (0..W-1, 0..H-1).
bool touched(int &x, int &y);

}  // namespace hal

// Per-board layout. The photo is requested from the bridge at exactly W x IMG_H.
namespace board {
#if defined(BOARD_CYD)
constexpr const char *NAME = "CYD News";
constexpr int W = 240, H = 320;
constexpr int IMG_H = 176;
constexpr int BAR_H = 3;           // progress bar under the photo
constexpr int PAD = 8;             // left/right margin for text
constexpr int TEXT_GAP = 10;       // photo -> first headline line
constexpr int TEXT_LINES = 6;
constexpr int FOOT_H = 18;         // distance from the bottom edge to the footer's top
constexpr int FOOT_LEFT_W = 140;   // room for "source | age"
constexpr int TOP_ZONE = 34;       // tapping this strip at the top switches feed
constexpr int BADGE_X = 6, BADGE_Y = 6, BADGE_H = 22, BADGE_PAD = 7;
constexpr size_t JPG_MAX = 56 * 1024;
#elif defined(BOARD_AMOLED206)
constexpr const char *NAME = "Watch News";
constexpr int W = 410, H = 502;
constexpr int IMG_H = 250;
constexpr int BAR_H = 4;
constexpr int PAD = 26;            // the panel has rounded corners
constexpr int TEXT_GAP = 14;
constexpr int TEXT_LINES = 6;
constexpr int FOOT_H = 40;
constexpr int FOOT_LEFT_W = 230;
constexpr int TOP_ZONE = 70;
constexpr int BADGE_X = 22, BADGE_Y = 20, BADGE_H = 38, BADGE_PAD = 14;
constexpr size_t JPG_MAX = 128 * 1024;  // lives in PSRAM
#else
#error "Define BOARD_CYD or BOARD_AMOLED206 (see platformio.ini)"
#endif
}  // namespace board
