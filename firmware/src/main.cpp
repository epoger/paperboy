// CYD News - Google News headlines + pictures on the ESP32-2432S028R.
//
// The CYD talks to bridge/server.py (running on a PC / Raspberry Pi on your LAN),
// which turns Google News into a small JSON feed and ESP32-friendly JPEGs.
//
// Screen (320x240, landscape):
//   +--------------------------------+
//   | [Top stories >]                |  <- tap the label: next feed
//   |          320x144 photo         |
//   |                                |
//   |================ progress =====|
//   | Headline text, word-wrapped    |
//   | up to four lines               |
//   | Source | 2h            3/12    |
//   +--------------------------------+
// Touch: left third = previous, right third = next, middle = pause / resume.

#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <SPI.h>
#include <TFT_eSPI.h>
#include <TJpg_Decoder.h>
#include <WiFi.h>
#include <XPT2046_Touchscreen.h>
#include <vector>

#include "config.h"

// ---- hardware ------------------------------------------------------------
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

// ---- layout --------------------------------------------------------------
static const int SCR_W = 320, SCR_H = 240;
static const int IMG_H = 144;                 // must match IMG_H in bridge/server.py
static const int BAR_Y = IMG_H, BAR_H = 3;    // progress bar sits under the photo
static const int TEXT_X = 8, TEXT_Y = IMG_H + 8, TEXT_W = SCR_W - 16, TEXT_LINES = 4;
static const int FOOT_Y = SCR_H - 17;
static const size_t JPG_MAX = 56 * 1024;

TFT_eSPI tft;
XPT2046_Touchscreen ts(TOUCH_CS, TOUCH_IRQ);

uint16_t C_BG, C_TEXT, C_DIM, C_ACCENT;

struct Item {
  String id, title, source, age, img;
};
std::vector<Item> items;
String feedLabel;
int feedIdx = 0;       // which entry of FEEDS[]
int cur = 0;           // current slide
bool paused = false;
uint32_t slideShownAt = 0, lastFeedAt = 0, lastBar = 0;
int barPx = 0;
static uint8_t jpgBuf[JPG_MAX];

// ---- helpers ---------------------------------------------------------------
bool tftOutput(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t *bmp) {
  if (y >= tft.height()) return false;
  tft.pushImage(x, y, w, h, bmp);
  return true;
}

void status(const String &line1, const String &line2 = "", uint16_t col = TFT_WHITE) {
  tft.fillScreen(C_BG);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(col, C_BG);
  tft.setFreeFont(&FreeSansBold12pt7b);
  tft.drawString(line1, SCR_W / 2, SCR_H / 2 - 14);
  tft.setTextFont(2);
  tft.setTextColor(C_DIM, C_BG);
  tft.drawString(line2, SCR_W / 2, SCR_H / 2 + 16);
  Serial.printf("[status] %s %s\n", line1.c_str(), line2.c_str());
}

// Greedy word-wrap using the currently selected font.
std::vector<String> wrap(const String &text, int maxW, int maxLines) {
  std::vector<String> lines;
  String line;
  int i = 0, len = text.length();
  while (i < len) {
    int j = text.indexOf(' ', i);
    if (j < 0) j = len;
    String word = text.substring(i, j);
    i = j + 1;
    if (!word.length()) continue;
    String trial = line.length() ? line + " " + word : word;
    if (tft.textWidth(trial) <= maxW) {
      line = trial;
    } else {
      if (line.length()) lines.push_back(line);
      line = word;
      while (line.length() > 1 && tft.textWidth(line) > maxW) line.remove(line.length() - 1);
    }
  }
  if (line.length()) lines.push_back(line);
  if ((int)lines.size() > maxLines) {
    lines.resize(maxLines);
    String &last = lines.back();
    while (last.length() > 1 && tft.textWidth(last + "...") > maxW) last.remove(last.length() - 1);
    last += "...";
  }
  return lines;
}

// ---- network ---------------------------------------------------------------
bool connectWiFi() {
  if (WiFi.status() == WL_CONNECTED) return true;
  status("Connecting to WiFi", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  for (int i = 0; i < 60 && WiFi.status() != WL_CONNECTED; i++) delay(250);
  if (WiFi.status() != WL_CONNECTED) {
    status("WiFi failed", "Check WIFI_SSID / WIFI_PASS in config.h", TFT_ORANGE);
    return false;
  }
  Serial.printf("WiFi ok, IP %s\n", WiFi.localIP().toString().c_str());
  return true;
}

bool fetchFeed() {
  if (!connectWiFi()) return false;
  String url = String(BRIDGE_URL) + "/feed.json?" + FEEDS[feedIdx];
  HTTPClient http;
  http.setTimeout(20000);  // first request can take a few seconds while the bridge warms up
  http.begin(url);
  int code = http.GET();
  if (code != 200) {
    Serial.printf("feed HTTP %d (%s)\n", code, url.c_str());
    http.end();
    return false;
  }
  String body = http.getString();
  http.end();

  JsonDocument doc;
  if (deserializeJson(doc, body)) return false;

  String curId = (cur < (int)items.size()) ? items[cur].id : "";
  items.clear();
  for (JsonObject o : doc["items"].as<JsonArray>()) {
    Item it;
    it.id = o["id"] | "";
    it.title = o["t"] | "";
    it.source = o["s"] | "";
    it.age = o["a"] | "";
    it.img = o["img"] | "";
    items.push_back(it);
    if (items.size() >= 20) break;
  }
  feedLabel = (const char *)(doc["label"] | "News");
  // keep the same story on screen after a background refresh if it's still there
  cur = 0;
  for (size_t i = 0; i < items.size(); i++)
    if (items[i].id == curId) cur = i;
  lastFeedAt = millis();
  Serial.printf("feed '%s': %d items\n", feedLabel.c_str(), (int)items.size());
  return !items.empty();
}

// Download one slide image into jpgBuf. Returns its size, or 0 if unavailable.
size_t fetchImage(const String &path) {
  if (path.isEmpty()) return 0;
  HTTPClient http;
  http.setTimeout(8000);
  http.begin(String(BRIDGE_URL) + path);
  int code = http.GET();
  size_t size = (code == 200) ? http.getSize() : 0;
  if (size == 0 || size > JPG_MAX) {
    if (code != 404) Serial.printf("image %s -> HTTP %d size %u\n", path.c_str(), code, (unsigned)size);
    http.end();
    return 0;
  }
  WiFiClient *s = http.getStreamPtr();
  size_t got = 0;
  uint32_t t = millis();
  while (got < size && (http.connected() || s->available()) && millis() - t < 8000) {
    int a = s->available();
    if (a) {
      got += s->readBytes(jpgBuf + got, min((size_t)a, size - got));
      t = millis();
    } else {
      delay(1);
    }
  }
  http.end();
  return got == size ? size : 0;
}

// ---- drawing ---------------------------------------------------------------
void drawBadge() {
  String label = feedLabel + " >";
  tft.setTextFont(2);
  int w = min(tft.textWidth(label) + 14, 230);
  tft.fillRoundRect(6, 6, w, 22, 5, tft.color565(10, 10, 14));
  tft.setTextDatum(ML_DATUM);
  tft.setTextColor(TFT_WHITE);
  tft.drawString(label, 13, 18);
}

void drawFooter() {
  const Item &it = items[cur];
  tft.fillRect(0, FOOT_Y - 1, SCR_W, SCR_H - FOOT_Y + 1, C_BG);
  tft.setTextFont(2);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(C_ACCENT, C_BG);
  String left = it.source;
  if (it.age.length()) left += "  |  " + it.age;
  while (left.length() > 1 && tft.textWidth(left) > 215) left.remove(left.length() - 1);
  tft.drawString(left, TEXT_X, FOOT_Y);
  tft.setTextDatum(TR_DATUM);
  tft.setTextColor(C_DIM, C_BG);
  tft.drawString(String(paused ? "paused  " : "") + String(cur + 1) + "/" + String(items.size()),
                 SCR_W - TEXT_X, FOOT_Y);
}

void drawPlaceholder(const Item &it) {
  uint32_t h = 2166136261u;
  for (size_t i = 0; i < it.source.length(); i++) h = (h ^ (uint8_t)it.source[i]) * 16777619u;
  tft.fillRect(0, 0, SCR_W, IMG_H, tft.color565(25 + (h & 0x3F), 25 + ((h >> 6) & 0x3F), 55 + ((h >> 12) & 0x4F)));
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_WHITE);
  tft.setFreeFont(&FreeSansBold24pt7b);
  String initial = it.source.length() ? it.source.substring(0, 1) : "N";
  initial.toUpperCase();
  tft.drawString(initial, SCR_W / 2, IMG_H / 2);
}

void drawSlide() {
  if (items.empty()) return;
  const Item &it = items[cur];

  // text first (instant), so the screen responds before the picture arrives
  tft.fillRect(0, BAR_Y, SCR_W, SCR_H - BAR_Y, C_BG);
  tft.setFreeFont(&FreeSansBold9pt7b);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(C_TEXT, C_BG);
  auto lines = wrap(it.title, TEXT_W, TEXT_LINES);
  int lh = tft.fontHeight();
  for (size_t i = 0; i < lines.size(); i++) tft.drawString(lines[i], TEXT_X, TEXT_Y + i * lh);
  drawFooter();

  size_t n = fetchImage(it.img);
  bool ok = n && TJpgDec.drawJpg(0, 0, jpgBuf, n) == 0;
  if (!ok) drawPlaceholder(it);
  drawBadge();

  slideShownAt = millis();
  barPx = 0;
}

void updateBar() {
  if (millis() - lastBar < 150 || items.empty()) return;
  lastBar = millis();
  int px = paused ? barPx : min(SCR_W, (int)((millis() - slideShownAt) * SCR_W / (SLIDE_SECONDS * 1000UL)));
  if (px > barPx) {
    tft.fillRect(barPx, BAR_Y, px - barPx, BAR_H, C_ACCENT);
    barPx = px;
  }
}

void showSlide(int idx) {
  if (items.empty()) return;
  cur = (idx + items.size()) % items.size();
  drawSlide();
}

void switchFeed() {
  feedIdx = (feedIdx + 1) % (sizeof(FEEDS) / sizeof(FEEDS[0]));
  status("Loading...", FEEDS[feedIdx]);
  items.clear();
  cur = 0;
  if (fetchFeed()) showSlide(0);
  else status("Can't reach the bridge", String(BRIDGE_URL), TFT_ORANGE);
}

// ---- touch -----------------------------------------------------------------
void handleTouch() {
  static bool was = false;
  static uint32_t lastTap = 0;
  bool now = ts.touched();
  if (now && !was && millis() - lastTap > 250) {
    TS_Point p = ts.getPoint();
    int x = constrain(map(p.x, TOUCH_X_MIN, TOUCH_X_MAX, 0, SCR_W - 1), 0, SCR_W - 1);
    int y = constrain(map(p.y, TOUCH_Y_MIN, TOUCH_Y_MAX, 0, SCR_H - 1), 0, SCR_H - 1);
    lastTap = millis();
    Serial.printf("tap raw(%d,%d) -> (%d,%d)\n", p.x, p.y, x, y);
    if (y < 34 && x < 240) switchFeed();
    else if (x < SCR_W / 3) showSlide(cur - 1);
    else if (x > SCR_W * 2 / 3) showSlide(cur + 1);
    else {
      paused = !paused;
      if (!paused) slideShownAt = millis() - (uint32_t)barPx * SLIDE_SECONDS * 1000UL / SCR_W;
      drawFooter();
    }
  }
  was = now;
}

// ---- main ------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  // the CYD's RGB LED is active-low and very bright: switch it off
  for (int pin : {4, 16, 17}) {
    pinMode(pin, OUTPUT);
    digitalWrite(pin, HIGH);
  }

  tft.init();
  tft.setRotation(SCREEN_ROTATION);
  tft.setSwapBytes(true);
  C_BG = tft.color565(14, 16, 22);
  C_TEXT = TFT_WHITE;
  C_DIM = tft.color565(140, 146, 160);
  C_ACCENT = tft.color565(66, 133, 244);

  // The display runs on HSPI (see USE_HSPI_PORT in platformio.ini); touch gets the default VSPI bus.
  SPI.begin(TOUCH_SCLK, TOUCH_MISO, TOUCH_MOSI, TOUCH_CS);
  ts.begin();
  ts.setRotation(SCREEN_ROTATION);

  TJpgDec.setJpgScale(1);
  TJpgDec.setSwapBytes(true);
  TJpgDec.setCallback(tftOutput);

  status("CYD News", "starting...");
  if (connectWiFi()) {
    status("Loading headlines...", BRIDGE_URL);
    if (fetchFeed()) showSlide(0);
    else status("Can't reach the bridge", String(BRIDGE_URL), TFT_ORANGE);
  }
}

void loop() {
  handleTouch();

  if (items.empty()) {  // nothing to show yet: retry every 15 s
    static uint32_t lastTry = 0;
    if (millis() - lastTry > 15000) {
      lastTry = millis();
      if (fetchFeed()) showSlide(0);
    }
    return;
  }

  updateBar();

  if (!paused && millis() - slideShownAt >= SLIDE_SECONDS * 1000UL) {
    // on the last slide, refresh the feed if it's stale before wrapping around
    if (cur == (int)items.size() - 1 && millis() - lastFeedAt > FEED_REFRESH_MINUTES * 60000UL) {
      if (fetchFeed()) {
        showSlide(0);
        return;
      }
      lastFeedAt = millis() - (FEED_REFRESH_MINUTES * 60000UL) + 60000UL;  // retry in a minute
    }
    showSlide(cur + 1);
  }
}
