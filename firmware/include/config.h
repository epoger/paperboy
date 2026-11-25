#pragma once
// ---------------------------------------------------------------------------
//  CYD News settings - edit these three, then build & upload.
// ---------------------------------------------------------------------------

// 1. Your 2.4 GHz WiFi network (the ESP32 can't use 5 GHz).
#define WIFI_SSID "your-wifi-name"
#define WIFI_PASS "your-wifi-password"

// 2. Where bridge/server.py is running: the LAN IP of that PC (run `ipconfig`
//    on Windows) and the port. No trailing slash.
#define BRIDGE_URL "http://192.168.86.190:8080"

// 3. Which Google News feeds to cycle through. Tap the label in the top-left
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
#define SCREEN_ROTATION 1         // 1 = USB port on the right, 3 = USB port on the left
