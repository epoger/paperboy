// CYD News - Google News headlines + pictures on small ESP32 touch displays.
//   * "Cheap Yellow Display" ESP32-2432S028R  (240x320)
//   * Waveshare ESP32-S3-Touch-AMOLED-2.06    (410x502)
// Board specifics live in hal_cyd.cpp / hal_amoled206.cpp; layout sizes in hal.h.
//
// The device talks to bridge/server.py (running on a PC / Raspberry Pi on your LAN),
// which turns Google News into a small JSON feed and ESP32-friendly JPEGs.
//
// Screen (portrait):
//   +------------------------+
//   | [Top stories >]        |  <- tap the top strip: next feed
//   |                        |
//   |  photo (W x IMG_H)     |
//   |                        |
//   |======= progress =======|
//   | Headline text,         |
//   | word-wrapped over up   |
//   | to six lines           |
//   | Source | 2h      3/12  |
//   +------------------------+
// Touch: left third = previous, right third = next, middle = pause / resume.

#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <vector>

#include "config.h"
#include "hal.h"

using board::H;
using board::W;

static const int BAR_Y = board::IMG_H;
static const int TEXT_X = board::PAD, TEXT_Y = board::IMG_H + board::TEXT_GAP, TEXT_W = W - 2 * board::PAD;
static const int FOOT_Y = H - board::FOOT_H;

uint16_t C_BG, C_TEXT, C_DIM, C_ACCENT, C_BADGE;

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
uint8_t *jpgBuf = nullptr;

// ---- helpers ---------------------------------------------------------------
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
    if (hal::textWidth(trial) <= maxW) {
      line = trial;
    } else {
      if (line.length()) lines.push_back(line);
      line = word;
      while (line.length() > 1 && hal::textWidth(line) > maxW) line.remove(line.length() - 1);
    }
  }
  if (line.length()) lines.push_back(line);
  if ((int)lines.size() > maxLines) {
    lines.resize(maxLines);
    String &last = lines.back();
    while (last.length() > 1 && hal::textWidth(last + "...") > maxW) last.remove(last.length() - 1);
    last += "...";
  }
  return lines;
}

// Full-screen message, centred; both lines word-wrap to fit the narrow portrait screen.
void status(const String &line1, const String &line2 = "", uint16_t col = hal::WHITE) {
  hal::fillScreen(C_BG);
  hal::setFont(hal::FONT_TITLE);
  auto l1 = wrap(line1, W - 2 * board::PAD, 3);
  int h1 = hal::lineHeight();
  hal::setFont(hal::FONT_SMALL);
  auto l2 = wrap(line2, W - 2 * board::PAD, 4);
  int h2 = hal::lineHeight();
  int y = (H - ((int)l1.size() * h1 + 10 + (int)l2.size() * h2)) / 2;
  hal::setFont(hal::FONT_TITLE);
  for (auto &l : l1) {
    hal::drawText(l, W / 2, y, hal::TC, col, C_BG);
    y += h1;
  }
  y += 10;
  hal::setFont(hal::FONT_SMALL);
  for (auto &l : l2) {
    hal::drawText(l, W / 2, y, hal::TC, C_DIM, C_BG);
    y += h2;
  }
  Serial.printf("[status] %s %s\n", line1.c_str(), line2.c_str());
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
    status("WiFi failed", "Check WIFI_SSID / WIFI_PASS in config.h", hal::ORANGE);
    return false;
  }
  Serial.printf("WiFi ok, IP %s\n", WiFi.localIP().toString().c_str());
  return true;
}

bool fetchFeed() {
  if (!connectWiFi()) return false;
  // w/h tell the bridge what size to make the photos for this screen
  String url = String(BRIDGE_URL) + "/feed.json?" + FEEDS[feedIdx] + "&w=" + String(W) + "&h=" + String(board::IMG_H);
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
  if (size == 0 || size > board::JPG_MAX) {
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
  hal::setFont(hal::FONT_SMALL);
  int w = min(hal::textWidth(label) + 2 * board::BADGE_PAD, W - 2 * board::BADGE_X) & ~1;  // even: the AMOLED needs it
  hal::fillRoundRect(board::BADGE_X, board::BADGE_Y, w, board::BADGE_H, 5, C_BADGE);
  hal::drawText(label, board::BADGE_X + board::BADGE_PAD, board::BADGE_Y + board::BADGE_H / 2, hal::ML, hal::WHITE,
                C_BADGE);
}

void drawFooter() {
  const Item &it = items[cur];
  hal::fillRect(0, FOOT_Y - 2, W, H - FOOT_Y + 2, C_BG);  // even-aligned (the AMOLED needs that)
  hal::setFont(hal::FONT_SMALL);
  String left = it.source;
  if (it.age.length()) left += "  |  " + it.age;
  while (left.length() > 1 && hal::textWidth(left) > board::FOOT_LEFT_W) left.remove(left.length() - 1);
  hal::drawText(left, TEXT_X, FOOT_Y, hal::TL, C_ACCENT, C_BG);
  hal::drawText(String(paused ? "paused  " : "") + String(cur + 1) + "/" + String(items.size()), W - TEXT_X, FOOT_Y,
                hal::TR, C_DIM, C_BG);
}

void drawPlaceholder(const Item &it) {
  uint32_t h = 2166136261u;
  for (size_t i = 0; i < it.source.length(); i++) h = (h ^ (uint8_t)it.source[i]) * 16777619u;
  uint16_t col = hal::rgb(25 + (h & 0x3F), 25 + ((h >> 6) & 0x3F), 55 + ((h >> 12) & 0x4F));
  hal::fillRect(0, 0, W, board::IMG_H, col);
  hal::setFont(hal::FONT_BIG);
  String initial = it.source.length() ? it.source.substring(0, 1) : "N";
  initial.toUpperCase();
  hal::drawText(initial, W / 2, board::IMG_H / 2, hal::MC, hal::WHITE, col);
}

void drawSlide() {
  if (items.empty()) return;
  const Item &it = items[cur];

  // text first (instant), so the screen responds before the picture arrives
  hal::fillRect(0, BAR_Y, W, H - BAR_Y, C_BG);
  hal::setFont(hal::FONT_BODY);
  auto lines = wrap(it.title, TEXT_W, board::TEXT_LINES);
  int lh = hal::lineHeight();
  for (size_t i = 0; i < lines.size(); i++) hal::drawText(lines[i], TEXT_X, TEXT_Y + i * lh, hal::TL, C_TEXT, C_BG);
  drawFooter();

  size_t n = fetchImage(it.img);
  bool ok = n && hal::drawJpeg(jpgBuf, n, 0, 0);
  if (!ok) drawPlaceholder(it);
  drawBadge();

  slideShownAt = millis();
  barPx = 0;
}

void updateBar() {
  if (millis() - lastBar < 150 || items.empty()) return;
  lastBar = millis();
  int px = paused ? barPx : min(W, (int)((millis() - slideShownAt) * W / (SLIDE_SECONDS * 1000UL)));
  px &= ~1;  // even widths only: the AMOLED can't address odd-sized windows
  if (px > barPx) {
    hal::fillRect(barPx, BAR_Y, px - barPx, board::BAR_H, C_ACCENT);
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
  else status("Can't reach the bridge", String(BRIDGE_URL), hal::ORANGE);
}

// ---- touch -----------------------------------------------------------------
void handleTouch() {
  static bool was = false;
  static uint32_t lastTap = 0;
  int x = 0, y = 0;
  bool now = hal::touched(x, y);
  if (now && !was && millis() - lastTap > 250) {
    lastTap = millis();
    if (y < board::TOP_ZONE) switchFeed();
    else if (x < W / 3) showSlide(cur - 1);
    else if (x > W * 2 / 3) showSlide(cur + 1);
    else {
      paused = !paused;
      if (!paused) slideShownAt = millis() - (uint32_t)barPx * SLIDE_SECONDS * 1000UL / W;
      drawFooter();
    }
  }
  was = now;
}

// ---- main ------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  hal::begin();

#ifdef COLOR_TEST
  // Diagnostic build (pio run -e cyd-colortest): five vertical bars, left to right:
  // RED, GREEN, BLUE, WHITE, BLACK. Compare with what the screen actually shows.
  {
    const uint16_t bars[5] = {hal::rgb(255, 0, 0), hal::rgb(0, 255, 0), hal::rgb(0, 0, 255), hal::WHITE, 0};
    for (int i = 0; i < 5; i++) hal::fillRect(i * W / 5, 0, W / 5, H, bars[i]);
    Serial.println("COLOR TEST: bars should be red, green, blue, white, black");
    while (true) delay(1000);
  }
#endif

  C_BG = hal::rgb(14, 16, 22);
  C_TEXT = hal::WHITE;
  C_DIM = hal::rgb(140, 146, 160);
  C_ACCENT = hal::rgb(66, 133, 244);
  C_BADGE = hal::rgb(10, 10, 14);

  jpgBuf = (uint8_t *)(psramFound() ? ps_malloc(board::JPG_MAX) : malloc(board::JPG_MAX));
  if (!jpgBuf) {
    status("Out of memory", "Could not allocate the photo buffer", hal::ORANGE);
    while (true) delay(1000);
  }

  status(board::NAME, "starting...");
  if (connectWiFi()) {
    status("Loading headlines...", BRIDGE_URL);
    if (fetchFeed()) showSlide(0);
    else status("Can't reach the bridge", String(BRIDGE_URL), hal::ORANGE);
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
