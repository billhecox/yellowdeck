# CYD Stream Deck

A DIY stream deck built from a $10 **ESP32-2432S028R "Cheap Yellow Display" (CYD)** — a 2.8" 240×320 touchscreen ESP32 board. The screen shows a now-playing header (player status + track info) above 10 icon buttons (2 columns × 5 rows, portrait); tapping a button sends the button ID over USB serial to a small Python bridge on the PC, which runs the mapped action: media controls, launching apps, or any shell command.

**Everything about the buttons — label, icon, color, and action — lives in one JSON file on the PC.** The bridge pushes the layout to the device over serial, so you never reflash to reconfigure. Edit `streamdeck_config.json`, save, and the deck redraws within a second.

```
┌───────────────────┐
│ ▶ Track Title     │
│   Artist          │      CYD (ESP32 + touchscreen)
├───────────────────┤           │
│ ▶ Play  │ ⏭ Next  │      USB serial "BTN:<id>"
│ ⏮ Prev  │ 🔊 Vol + │           │
│ 🔉 Vol - │ 🔇 Mute  │      streamdeck_bridge.py ──▶ action
│ ⌨ Term  │ </> Code │           ▲
│ ♫ Music │ 🔒 Lock  │      "NOW:<state>:<title>:<artist>"
└───────────────────┘
```

## Features

- 10 touch buttons with color-coded icons drawn on-device (no bitmap assets)
- Now-playing header: play/pause/stop state plus track title and artist, updated live (~2 s)
- **Live reconfiguration** — labels, icons, colors, and actions hot-reload from JSON, no reflash
- Media controls that work without extra dependencies: MPRIS via D-Bus for play/next/prev, `wpctl`/`pactl`/`amixer` for volume (uses `playerctl` if you have it)
- Launch any shell command per button
- Auto-detects the serial port, auto-reconnects if the device is unplugged
- Touch debouncing, glitch filtering, and press highlight feedback
- Includes a 3-tap touch calibration sketch for panel variance

## Hardware

Just one board: the ESP32-2432S028R (search "Cheap Yellow Display" or "CYD"). ILI9341 TFT + XPT2046 resistive touch, connected to the PC over its USB port (which is also power and the serial link).

## Repo layout

```
cyd_streamdeck/cyd_streamdeck.ino   Firmware: buttons, icons, touch, serial protocol
cyd_touchcal/cyd_touchcal.ino      One-shot touch calibration sketch
TFT_eSPI_Setup_CYD.h               TFT_eSPI pin/driver setup for the CYD
streamdeck_bridge.py               PC bridge: serial listener + config pusher
streamdeck_config.json             All button definitions and actions
requirements.txt                   Python deps (pyserial)
```

## Quick start

### 1. Set up the toolchain

Install [arduino-cli](https://arduino.github.io/arduino-cli/) (or use the Arduino IDE), then:

```bash
arduino-cli core install esp32:esp32 \
  --additional-urls https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
arduino-cli lib install TFT_eSPI XPT2046_Touchscreen
```

TFT_eSPI needs to be told about the CYD's pinout:

1. Copy `TFT_eSPI_Setup_CYD.h` into `~/Arduino/libraries/TFT_eSPI/User_Setups/`
2. In `~/Arduino/libraries/TFT_eSPI/User_Setup_Select.h`, comment out the active `#include <User_Setup.h>` line and add:
   ```cpp
   #include <User_Setups/TFT_eSPI_Setup_CYD.h>
   ```

### 2. Flash the firmware

```bash
arduino-cli compile --fqbn esp32:esp32:esp32 cyd_streamdeck
arduino-cli upload  --fqbn esp32:esp32:esp32 -p /dev/ttyUSB0 cyd_streamdeck
```

If the upload can't enter the bootloader: hold **BOOT**, tap **RST**, release **BOOT**, retry.

On Linux you need to be in the `dialout` group for `/dev/ttyUSB0` (`sudo usermod -aG dialout $USER`, then log out/in).

### 3. Run the bridge

Linux/macOS:

```bash
python3 -m venv .venv
.venv/bin/pip install -r requirements.txt
.venv/bin/python streamdeck_bridge.py
```

Windows (PowerShell; install the [CH340 driver](https://www.wch-ic.com/downloads/CH341SER_ZIP.html) first if the board doesn't show up as a COM port):

```powershell
py -m venv .venv
.venv\Scripts\pip install -r requirements.txt
.venv\Scripts\python streamdeck_bridge.py
```

You should see `Pushed layout for 10 buttons` and the deck redraw with your configured layout. Tap away.

## Configuring buttons

Everything is in `streamdeck_config.json`. Each button (IDs `0`–`9`, left-to-right then top-to-bottom) looks like:

```json
"6": {
  "label": "Term",
  "icon": "term",
  "color": "AAFF33",
  "type": "launch",
  "command": "gnome-terminal"
}
```

| Field | Meaning |
|---|---|
| `label` | Text shown on the button. Max 12 chars (≈8 fit comfortably). Colons are replaced with spaces. |
| `icon` | One of the built-in icon names below. Unknown names fall back to `gear`. |
| `color` | 24-bit hex RGB (`#` optional). Used for the icon, border, and pressed fill. |
| `type` | `media` or `launch`. |
| `action` | For `media`: `play_pause`, `next`, `previous`, `volume_up`, `volume_down`, `mute`. |
| `command` | For `launch`: any shell command, run detached from the bridge. Commands go to the platform shell, so use OS-appropriate ones (e.g. `xdg-open …` on Linux, `start …` on Windows). |

Top-level keys: `port` (`"auto"` scans `/dev/ttyUSB*` and `/dev/ttyACM*`, or set an explicit path) and `baud` (leave at `115200`).

**Save the file and the deck updates live** — the bridge watches the file's mtime (~1 s) and re-pushes the layout. No restart, no reflash.

### Built-in icons

| Name | Glyph | Typical use |
|---|---|---|
| `play` | Play triangle + pause bars | Play/pause toggle |
| `next` | Skip-forward | Next track |
| `prev` | Skip-back | Previous track |
| `volup` | Speaker with + | Volume up |
| `voldn` | Speaker with – | Volume down |
| `mute` | Speaker with × | Mute toggle |
| `term` | Terminal window with prompt | Open a terminal |
| `web` | Globe | Browser / URL |
| `files` | Folder | File manager |
| `code` | `</>` brackets | Editor / IDE |
| `music` | Eighth note | Music app |
| `mail` | Envelope | Email |
| `shot` | Camera (still) | Screenshot |
| `calc` | Calculator | Calculator |
| `lock` | Padlock | Lock screen |
| `gear` | Gear | Settings; also the fallback for unknown names |
| `mic` | Microphone on stand | Mic mute toggle (meetings) |
| `cam` | Video camera | Webcam toggle |
| `chat` | Speech bubble | Chat / messaging app |
| `game` | Gamepad | Games / Steam |
| `rec` | Dot in ring | Start recording |
| `stop` | Filled square | Stop playback/recording |
| `star` | Four-point sparkle | Favorite / bookmark |
| `heart` | Heart | Like / favorite |
| `sun` | Sun with rays | Brightness / light mode |
| `moon` | Crescent moon | Night mode / sleep |
| `search` | Magnifying glass | Search / launcher |
| `bell` | Bell | Notifications / do-not-disturb |
| `home` | House | Home / dashboard |
| `power` | Power symbol | Shutdown / suspend |

Icons are drawn with TFT primitives in the firmware's `drawIcon()`. Adding a new one is ~10 lines of C++ in `cyd_streamdeck.ino` (draw inside a 22×22 box centered on `cx,cy`, add a name to `ICON_NAMES`), and then it's usable from the JSON like any other.

### Example: repurpose a button

Change button 9 from Lock to a screenshot key — edit the JSON and save:

```json
"9": {
  "label": "Shot",
  "icon": "shot",
  "color": "DDDDDD",
  "type": "launch",
  "command": "gdbus call --session --dest org.gnome.Shell.Screenshot --object-path /org/gnome/Shell/Screenshot --method org.gnome.Shell.Screenshot.InteractiveScreenshot"
}
```

## Serial protocol

Plain text lines at 115200 baud, so you can drive the deck from anything (or debug with a serial monitor):

| Direction | Line | Meaning |
|---|---|---|
| device → PC | `STREAMDECK READY 10` | Booted; layout is at firmware defaults, push config |
| device → PC | `BTN:<id>` | Button `<id>` pressed (sent once per press, on touch-down) |
| device → PC | `OK:CFG <id>` / `ERR:<msg>` | Config line accepted / rejected |
| PC → device | `CFG:<id>:<icon>:<label>:<rrggbb>` | Set one button's icon, label, color; redraws immediately |
| PC → device | `NOW:<state>:<title>:<artist>` | Update the header; `<state>` is `play`/`pause`/`stop` |
| PC → device | `PING` | Ask the device to re-announce `STREAMDECK READY` |

## Touch calibration

Resistive panels vary board-to-board. If taps land on the wrong button:

1. Flash `cyd_touchcal`:
   ```bash
   arduino-cli compile --fqbn esp32:esp32:esp32 cyd_touchcal
   arduino-cli upload  --fqbn esp32:esp32:esp32 -p /dev/ttyUSB0 cyd_touchcal
   ```
2. Tap the 3 crosshairs; the sketch prints `CAL<n> <rawx> <rawy>` per tap over serial.
3. Targets are at screen (30,30), (210,30), (30,290). Fit the raw values linearly and update the `TOUCH_RX_*` / `TOUCH_RY_*` defines in `cyd_streamdeck.ino` (comments there explain which raw axis maps to which screen axis), then reflash the deck.

## Run at login (systemd user service)

```bash
mkdir -p ~/.config/systemd/user
cat > ~/.config/systemd/user/cyd-streamdeck.service <<'EOF'
[Unit]
Description=CYD stream deck serial bridge

[Service]
WorkingDirectory=%h/cyd-streamdeck
ExecStart=%h/cyd-streamdeck/.venv/bin/python streamdeck_bridge.py
Restart=on-failure

[Install]
WantedBy=default.target
EOF
systemctl --user enable --now cyd-streamdeck
```

Adjust `WorkingDirectory`/`ExecStart` if the repo lives elsewhere. Logs: `journalctl --user -u cyd-streamdeck -f`.

## Troubleshooting

- **Nothing on screen** — check the TFT_eSPI setup steps; the wrong `User_Setup` is the #1 cause. Backlight is GPIO 21.
- **Taps hit the wrong button** — run the calibration sketch (above).
- **`Permission denied: /dev/ttyUSB0`** — join `dialout` group, or `sg dialout -c '...'` for the current session.
- **Bridge says "no media player running"** — play/next/prev need an MPRIS-capable player running (Spotify, Firefox/Chrome media, VLC, …), or install `playerctl`.
- **Deck shows default labels** — the firmware booted without the bridge running; start the bridge and it re-pushes on the `STREAMDECK READY` banner.
- **Boot loop / brownout** — use a short, data-capable USB cable; CYDs are picky about power.
- **Garbled display** — lower `SPI_FREQUENCY` in `TFT_eSPI_Setup_CYD.h` (try 27 MHz).

## Notes

- Firmware defaults (the layout shown before the bridge connects) are in the `buttons[]` array in `cyd_streamdeck.ino`; they're cosmetic — the JSON always wins once the bridge connects.
- The bridge runs on Linux and Windows. Media actions use playerctl/MPRIS + wpctl/pactl/amixer on Linux, and synthesized media-key presses on Windows (routed by the OS to the active media session). The now-playing header works on both: MPRIS metadata on Linux, the Windows media-session API via the `winsdk` package (installed automatically from `requirements.txt` on Windows). `launch` commands are passed to the platform shell, so they're OS-specific — Windows equivalents of the default config: `wt` (terminal), `explorer` (files), `start https://music.youtube.com` (URLs), `calc`, `rundll32 user32.dll,LockWorkStation` (lock). Port `"auto"` picks the first USB-serial adapter on either OS; set `"port": "COM3"`-style if you have several. PRs for a macOS media backend welcome.
