# CYD News

Google News headlines **and pictures** on small ESP32 touchscreens, used in portrait:

| Board                                                                   | Screen      | PlatformIO env     | Status                          |
|-------------------------------------------------------------------------|-------------|--------------------|---------------------------------|
| "Cheap Yellow Display" ESP32-2432S028R                                  | 2.8" 240x320 | `cyd` (default)    | tested on hardware              |
| Waveshare ESP32-S3-Touch-AMOLED-2.06 (watch)                            | 2.06" 410x502 AMOLED | `watch-amoled206` | tested on hardware |

```
Google News RSS ──► bridge/server.py (PC / Raspberry Pi) ──WiFi/HTTP──► CYD / watch
                    • finds each article's lead image
                    • shrinks it to the size the device asks for (baseline JPEG)
                    • serves compact JSON + images
```

**Why a bridge?** Google News' RSS feed contains only headlines — no images — and its article links are encrypted
redirects. Finding each story's photo means loading the publisher's page, then downloading and resizing a large image.
That is far too heavy for an ESP32, so a small Python server does it and hands the device ready-to-draw slides.
Each device asks for its own photo size (`?w=&h=`), so one bridge can serve a CYD and a watch at the same time.

## Screen & controls

| Tap                       | Does                                   |
|---------------------------|----------------------------------------|
| Label in top-left corner (top strip) | Next feed (Top → World → Tech → …) |
| Left third of the screen  | Previous story                         |
| Right third               | Next story                             |
| Middle                    | Pause / resume auto-advance            |

Stories auto-advance every 12 s (blue bar under the photo is the timer). The headline list refreshes every 10 min.
The bridge only passes on stories whose photo it could load: stories it can't get a picture for are skipped and
replaced by the next ones down Google's list. (If no pictures can be loaded at all, e.g. no internet, it falls back
to headlines with a coloured placeholder.)

## Setup

### 1. Run the bridge (on a PC that stays on while the device is in use)

```powershell
cd bridge
pip install -r requirements.txt
python server.py            # or just double-click run-bridge.bat
```

Open <http://localhost:8080> to preview what a CYD will show (add `?w=410&h=250` to preview the watch's photos).
Options: `--port`, `--count` (stories per feed), `--feed-wait` (max seconds to wait for images, default 15),
`--lang en-US`, `--country US`, `--refresh` (seconds), `--quality`. The first feed request after starting takes a few
seconds while images load.

Windows will ask to let Python through the firewall the first time — allow it on **Private** networks, otherwise the
device can't connect. Find the PC's LAN address with `ipconfig` (the "IPv4 Address").

### 2. Configure and flash

Copy [firmware/include/config_secrets.example.h](firmware/include/config_secrets.example.h) to
`firmware/include/config_secrets.h` (git-ignored) and fill in your WiFi name/password (2.4 GHz only) and the bridge's
`BRIDGE_URL` (e.g. `http://192.168.1.50:8080`). The list of feeds and other options are in
[firmware/include/config.h](firmware/include/config.h).

Install [PlatformIO](https://platformio.org/install) (VS Code extension, or `pip install platformio`), plug the board
in, then from the `firmware` folder:

```powershell
pio run -t upload                      # CYD (default environment)
pio run -e watch-amoled206 -t upload   # Waveshare AMOLED 2.06 watch
pio device monitor                     # optional serial log
```

If several serial devices are connected, add `--upload-port COMx`.

#### CYD notes

The default environment `cyd` is tested on a real board. If your screen stays white, your panel may be a different
controller: try `pio run -e cyd-st7789 -t upload`. If colours look inverted (cyan/magenta/yellow instead of
red/green/blue), swap `-DTFT_INVERSION_OFF=1` for `-DTFT_INVERSION_ON=1` in the `[env:cyd]` flags. To check colours,
`pio run -e cyd-colortest -t upload` shows bars that should read red, green, blue, white, black.

#### Waveshare ESP32-S3-Touch-AMOLED-2.06 notes

Built from Waveshare's published Arduino example (pins, the CO5300 display set-up with a 22-pixel column offset, and the
FT3168 touch registers) and tested on a real watch.

- The display is driven by [Arduino_GFX](https://github.com/moononournation/Arduino_GFX) over QSPI; touch is read
  directly from the FT3168 over I2C (`src/hal_amoled206.cpp`). The AXP2101 power chip is not touched: Waveshare's
  example doesn't initialise it for the display either.
- Brightness: `AMOLED_BRIGHTNESS` in `config.h` (0-255). Lower is easier on the battery.
- Serial output uses the native USB port (USB-CDC). If the upload can't find the board, hold **BOOT** while plugging
  in USB to enter download mode, then run the upload again.
- Check the colours/geometry first if something looks wrong: a shifted or clipped image usually means the column offset
  (`22` in `hal_amoled206.cpp`) or panel size doesn't match your unit; wrong touch positions can be seen in the
  serial log, which prints every new touch.

### Feeds

In `config.h`, each entry in `FEEDS` is sent to the bridge as-is:

- `topic=TOP`, `WORLD`, `NATION`, `BUSINESS`, `TECHNOLOGY`, `ENTERTAINMENT`, `SPORTS`, `SCIENCE`, `HEALTH`
- `q=some+search+terms` — any Google News search, e.g. `q=seattle+mariners`

For other regions, start the bridge with e.g. `python server.py --country GB --lang en-GB`.

## Troubleshooting

| Symptom                              | Fix |
|--------------------------------------|-----|
| "Can't reach the bridge"             | Check `BRIDGE_URL` IP, that the bridge is running, and the Windows firewall prompt. Test `http://<ip>:8080/feed.json` from another device. |
| CYD taps land in the wrong place     | Tweak `TOUCH_X_MIN/MAX`, `TOUCH_Y_MIN/MAX` at the top of `src/hal_cyd.cpp`; the serial monitor prints raw and mapped coordinates for every tap. |
| CYD screen upside-down               | Set `SCREEN_ROTATION` to `2` in `config.h` (portrait only: `0` or `2`). |
| Photos look garbled or cut off       | The bridge is an old version. Restart `bridge/server.py` — it must support `?w=&h=` photo sizes. |
| Stories from NYT, Reuters, AP… never appear | Those publishers refuse automated requests, so no picture is available and the bridge skips them (it deliberately doesn't work around the block). |
| A feed has fewer than 12 stories     | Google's list ran out of stories with loadable images, or `--feed-wait` expired. Raise `--feed-wait` or lower `--count`. |
| Non-English characters missing       | The display fonts are ASCII-only; the bridge folds accents to plain letters and drops anything else. |

## Layout

```
bridge/server.py             RSS → JSON + JPEG slides (Python: requests, pillow, googlenewsdecoder)
firmware/platformio.ini      one environment per board; all pin/driver settings live here
firmware/include/config.h    feeds and options
firmware/include/config_secrets.h   WiFi, bridge address (git-ignored; see config_secrets.example.h)
firmware/src/main.cpp        the app (same for every board)
firmware/src/hal.h           what the app needs from a board + per-board layout sizes
firmware/src/hal_cyd.cpp     CYD:   TFT_eSPI display + XPT2046 touch
firmware/src/hal_amoled206.cpp  Watch: Arduino_GFX CO5300 display + FT3168 touch
firmware/src/fonts/          Adafruit-GFX format fonts used on the watch (FreeFont, copied from TFT_eSPI)
```

Adding another board means writing one more `hal_<board>.cpp` and a PlatformIO environment.

Headlines and links belong to Google News and the publishers; this is for personal, non-commercial display use.
The `FreeSans*` font data is from the GNU FreeFont project as packaged for Adafruit GFX / TFT_eSPI.
