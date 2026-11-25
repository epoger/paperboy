# CYD News

Google News headlines **and pictures** on the ESP32 "Cheap Yellow Display" (ESP32-2432S028R, 2.8" 240x320 touchscreen, used in portrait).

```
Google News RSS ──► bridge/server.py (PC / Raspberry Pi) ──WiFi/HTTP──► CYD
                    • finds each article's lead image
                    • shrinks it to a 240x176 baseline JPEG
                    • serves compact JSON + images
```

**Why a bridge?** Google News' RSS feed contains only headlines — no images — and its article links are encrypted
redirects. Finding each story's photo means loading the publisher's page, then downloading and resizing a large image.
That is far too heavy for the ESP32, so a small Python server does it and hands the CYD ready-to-draw slides.

## Screen & controls

| Tap                       | Does                                   |
|---------------------------|----------------------------------------|
| Label in top-left corner  | Next feed (Top → World → Tech → …)     |
| Left third of the screen  | Previous story                         |
| Right third               | Next story                             |
| Middle                    | Pause / resume auto-advance            |

Stories auto-advance every 12 s (blue bar under the photo is the timer). The headline list refreshes every 10 min.
The bridge only passes on stories whose photo it could load: stories it can't get a picture for are skipped and
replaced by the next ones down Google's list. (If no pictures can be loaded at all, e.g. no internet, it falls back
to headlines with a coloured placeholder.)

## Setup

### 1. Run the bridge (on a PC that stays on while the CYD is in use)

```powershell
cd bridge
pip install -r requirements.txt
python server.py            # or just double-click run-bridge.bat
```

Open <http://localhost:8080> to preview exactly what the CYD will show. Options: `--port`, `--count`
(stories per feed), `--feed-wait` (max seconds to wait for images, default 15), `--lang en-US`, `--country US`,
`--refresh` (seconds), `--quality`. The first feed request after starting takes a few seconds while images load.

Windows will ask to let Python through the firewall the first time — allow it on **Private** networks, otherwise the
CYD can't connect. Find the PC's LAN address with `ipconfig` (the "IPv4 Address").

### 2. Configure and flash the CYD

Edit [firmware/include/config.h](firmware/include/config.h): WiFi name/password (2.4 GHz only), the bridge's
`BRIDGE_URL` (e.g. `http://192.168.1.50:8080`), and the list of feeds.

Install [PlatformIO](https://platformio.org/install) (VS Code extension, or `pip install platformio`), plug in the CYD, then:

```powershell
cd firmware
pio run -t upload         # builds the default environment, cyd (ILI9341, colour inversion on)
pio device monitor        # optional serial log
```

The default environment `cyd` is tested on a real board. If your screen stays white, your panel may be a different
controller: try `pio run -e cyd-st7789 -t upload`. If colours look inverted (cyan/magenta/yellow instead of
red/green/blue), swap `-DTFT_INVERSION_OFF=1` for `-DTFT_INVERSION_ON=1` in the `[env:cyd]` flags. To check colours,
`pio run -e cyd-colortest -t upload` shows bars that should read red, green, blue, white, black.

### Feeds

In `config.h`, each entry in `FEEDS` is sent to the bridge as-is:

- `topic=TOP`, `WORLD`, `NATION`, `BUSINESS`, `TECHNOLOGY`, `ENTERTAINMENT`, `SPORTS`, `SCIENCE`, `HEALTH`
- `q=some+search+terms` — any Google News search, e.g. `q=seattle+mariners`

For other regions, start the bridge with e.g. `python server.py --country GB --lang en-GB`.

## Troubleshooting

| Symptom                              | Fix |
|--------------------------------------|-----|
| "Can't reach the bridge"             | Check `BRIDGE_URL` IP, that the bridge is running, and the Windows firewall prompt. Test `http://<ip>:8080/feed.json` from another device. |
| Taps land in the wrong place         | Tweak `TOUCH_X_MIN/MAX`, `TOUCH_Y_MIN/MAX` at the top of `src/main.cpp`; the serial monitor prints raw and mapped coordinates for every tap. |
| Screen upside-down                   | Set `SCREEN_ROTATION` to `2` in `config.h` (portrait only: `0` or `2`). |
| Photos look garbled or cut off after switching to portrait | The bridge is still the old landscape version. Restart `bridge/server.py` — the firmware and bridge must agree on the 240x176 photo size. |
| Stories from NYT, Reuters, AP… never appear | Those publishers refuse automated requests, so no picture is available and the bridge skips them (it deliberately doesn't work around the block). |
| A feed has fewer than 12 stories     | Google's list ran out of stories with loadable images, or `--feed-wait` expired. Raise `--feed-wait` or lower `--count`. |
| Non-English characters missing       | The display font is ASCII-only; the bridge folds accents to plain letters and drops anything else. |

## Layout

```
bridge/server.py          RSS → JSON + JPEG slides (Python: requests, pillow, googlenewsdecoder)
firmware/platformio.ini   build config incl. all TFT_eSPI pin settings (no library edits needed)
firmware/include/config.h WiFi, bridge address, feeds
firmware/src/main.cpp     the app
```

Headlines and links belong to Google News and the publishers; this is for personal, non-commercial display use.
