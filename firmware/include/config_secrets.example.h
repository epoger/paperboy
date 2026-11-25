#pragma once
// Template for config_secrets.h, which is git-ignored.
// Copy this file to config_secrets.h and fill in your own values.

// Your 2.4 GHz WiFi network (the ESP32 can't use 5 GHz).
#define WIFI_SSID "your-wifi-name"
#define WIFI_PASS "your-wifi-password"

// Where bridge/server.py is running: the LAN IP of that PC (run `ipconfig`
// on Windows) and the port (as passed into the bridge --port flag). No trailing slash.
#define BRIDGE_URL "http://192.168.1.50:9412"
