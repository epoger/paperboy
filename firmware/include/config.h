#pragma once
// ---------------------------------------------------------------------------
//  CYD News settings - edit these, then build & upload.
// ---------------------------------------------------------------------------

// 1. WIFI_SSID, WIFI_PASS and BRIDGE_URL live in config_secrets.h (git-ignored).
#if __has_include("config_secrets.h")
#include "config_secrets.h"
#else
#error "Missing firmware/include/config_secrets.h - copy config_secrets.example.h to config_secrets.h and fill it in."
#endif

// 2. Which Google News feeds to cycle through. Tap the label in the top-left
//    corner of the screen to jump to the next one. Each entry is the query
//    string sent to the bridge:
//      "topic=TOP|WORLD|NATION|BUSINESS|TECHNOLOGY|ENTERTAINMENT|SPORTS|SCIENCE|HEALTH"
//      "q=your+search+terms"        (any Google News search, spaces as '+')
static const char *const FEEDS[] = {
    "topic=TOP",
    "topic=WORLD",
    "topic=TECHNOLOGY",
    "topic=BUSINESS",
    "topic=SCIENCE",
    "topic=SPORTS",
};

// ---------------------------------------------------------------------------
//  Optional tweaks
// ---------------------------------------------------------------------------
#define SLIDE_SECONDS 12          // auto-advance interval
#define FEED_REFRESH_MINUTES 10   // how often to re-download the headline list
#define SCREEN_ROTATION 0         // CYD only. Portrait: 0, or 2 to flip it upside-down
#define AMOLED_BRIGHTNESS 200     // Waveshare watch only. 0-255 (lower = longer battery life)
