#!/usr/bin/env python3
"""PC-side bridge for the YellowDeck.

Listens on the CYD's USB serial port for "BTN:<id>" lines and runs the
action mapped to that button in yellowdeck_config.json.

On connect (and whenever yellowdeck_config.json changes) the bridge pushes
each button's label/icon/color to the firmware as
"CFG:<id>:<icon>:<label>:<rrggbb>" lines, so the deck layout is fully
driven by the JSON file — no reflash needed. It also polls the active
media player (~2 s) and pushes "NOW:<state>:<title>:<artist>" lines that
the firmware shows in its now-playing header.

Action types:
  media:  play_pause | next | previous | volume_up | volume_down | mute
  launch: any shell command (runs detached)

Cross-platform: on Linux, media playback uses playerctl if installed
(falling back to raw MPRIS D-Bus calls) and volume uses wpctl/pactl/amixer.
On Windows, all six media actions are synthesized media-key presses, which
the OS routes to the active media session — no player discovery needed.
"""

import json
import shutil
import subprocess
import sys
import time
from pathlib import Path

import serial
from serial.tools import list_ports

IS_WINDOWS = sys.platform == "win32"

# When frozen by PyInstaller, __file__ points into the temp extraction dir;
# keep the config next to the exe instead so users can find and edit it.
APP_DIR = (
    Path(sys.executable).parent
    if getattr(sys, "frozen", False)
    else Path(__file__).resolve().parent
)
CONFIG_PATH = APP_DIR / "yellowdeck_config.json"


def default_config():
    """Starter layout, with launch commands matching the current OS."""
    def launch(linux, windows):
        return windows if IS_WINDOWS else linux

    media = [
        ("Play", "play", "22CC44", "play_pause"),
        ("Next", "next", "22CC44", "next"),
        ("Prev", "prev", "22CC44", "previous"),
        ("Vol +", "volup", "22AAFF", "volume_up"),
        ("Vol -", "voldn", "22AAFF", "volume_down"),
        ("Mute", "mute", "FF3311", "mute"),
    ]
    launches = [
        ("Term", "term", "AAFF33", launch("gnome-terminal", "wt")),
        ("Code", "code", "3BB3FF", "code"),
        ("Music", "music", "FF1493",
         launch("xdg-open https://music.youtube.com",
                "start https://music.youtube.com")),
        ("Lock", "lock", "FFEE00",
         launch("loginctl lock-session",
                "rundll32 user32.dll,LockWorkStation")),
    ]
    buttons = {}
    for i, (label, icon, color, action) in enumerate(media):
        buttons[str(i)] = {"label": label, "icon": icon, "color": color,
                           "type": "media", "action": action}
    for i, (label, icon, color, command) in enumerate(launches, start=len(media)):
        buttons[str(i)] = {"label": label, "icon": icon, "color": color,
                           "type": "launch", "command": command}
    return {"port": "auto", "baud": 115200, "buttons": buttons}


def load_config():
    if not CONFIG_PATH.exists():
        CONFIG_PATH.write_text(json.dumps(default_config(), indent=2) + "\n")
        print(f"Created default config: {CONFIG_PATH}")
        print("Edit it to customize buttons; changes apply live.")
    with open(CONFIG_PATH) as f:
        cfg = json.load(f)
    # Validate the shape so a half-edited file can't crash the push loop.
    buttons = cfg.get("buttons", {})
    if not isinstance(buttons, dict):
        raise ValueError('"buttons" must be an object')
    for bid, entry in buttons.items():
        if not bid.isdigit():
            raise ValueError(f"button id {bid!r} is not a number")
        if not isinstance(entry, dict):
            raise ValueError(f"button {bid} must be an object")
    return cfg


def find_port(configured):
    if configured and configured != "auto":
        return configured
    ports = sorted(list_ports.comports(), key=lambda p: p.device)
    # Prefer USB-serial adapters (the CYD's CH340 shows up with a USB VID);
    # motherboard COM ports and such report no VID.
    usb = [p for p in ports if p.vid is not None]
    picked = usb or ports
    return picked[0].device if picked else None


def run_detached(command):
    if IS_WINDOWS:
        kwargs = {
            "creationflags": subprocess.CREATE_NO_WINDOW
            | subprocess.CREATE_NEW_PROCESS_GROUP
        }
    else:
        kwargs = {"start_new_session": True}
    subprocess.Popen(
        command,
        shell=True,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        **kwargs,
    )


def push_config(ser, config):
    """Send each button's label/icon/color to the firmware."""
    for bid, entry in sorted(
        config.get("buttons", {}).items(), key=lambda kv: int(kv[0])
    ):
        icon = entry.get("icon", "gear")
        label = entry.get("label", "")[:12].replace(":", " ")
        color = entry.get("color", "CCCCCC").lstrip("#")
        line = f"CFG:{bid}:{icon}:{label}:{color}\n"
        ser.write(line.encode())
        time.sleep(0.03)  # let the firmware redraw between lines
    print(f"Pushed layout for {len(config.get('buttons', {}))} buttons")


# Windows virtual-key codes for the media keys.
MEDIA_VK = {
    "play_pause": 0xB3,   # VK_MEDIA_PLAY_PAUSE
    "next": 0xB0,         # VK_MEDIA_NEXT_TRACK
    "previous": 0xB1,     # VK_MEDIA_PREV_TRACK
    "volume_up": 0xAF,    # VK_VOLUME_UP
    "volume_down": 0xAE,  # VK_VOLUME_DOWN
    "mute": 0xAD,         # VK_VOLUME_MUTE
}


def media_key(action):
    """Tap a Windows media key; the OS delivers it to the active session."""
    import ctypes

    vk = MEDIA_VK[action]
    ctypes.windll.user32.keybd_event(vk, 0, 0, 0)
    ctypes.windll.user32.keybd_event(vk, 0, 2, 0)  # KEYEVENTF_KEYUP


def mpris_players():
    """List MPRIS bus names of running media players."""
    out = subprocess.run(
        ["dbus-send", "--session", "--print-reply",
         "--dest=org.freedesktop.DBus", "/org/freedesktop/DBus",
         "org.freedesktop.DBus.ListNames"],
        capture_output=True, text=True,
    ).stdout
    return [
        line.split('"')[1]
        for line in out.splitlines()
        if 'org.mpris.MediaPlayer2.' in line
    ]


def mpris_prop(player, prop):
    """Read one org.mpris.MediaPlayer2.Player property; '' on failure."""
    out = subprocess.run(
        ["dbus-send", "--session", "--print-reply", f"--dest={player}",
         "/org/mpris/MediaPlayer2", "org.freedesktop.DBus.Properties.Get",
         "string:org.mpris.MediaPlayer2.Player", f"string:{prop}"],
        capture_output=True, text=True,
    ).stdout
    if '"' in out:
        return out.split('"')[-2]
    return out.split()[-1] if out.split() else ""


def pick_player():
    """Prefer a player that will actually respond: Playing > Paused > CanPlay.

    Stale sessions (e.g. a browser whose media tab stopped) stay on the bus
    with PlaybackStatus "Stopped" and CanPlay false; they accept commands and
    ignore them, so they must be skipped.
    """
    best, best_rank = None, 0
    for name in mpris_players():
        status = mpris_prop(name, "PlaybackStatus")
        rank = {"Playing": 3, "Paused": 2}.get(status, 0)
        if rank == 0 and mpris_prop(name, "CanPlay") == "true":
            rank = 1
        if rank > best_rank:
            best, best_rank = name, rank
    return best


def mpris_metadata(player, key):
    """Extract one xesam:* string from the player's Metadata dict."""
    out = subprocess.run(
        ["dbus-send", "--session", "--print-reply", f"--dest={player}",
         "/org/mpris/MediaPlayer2", "org.freedesktop.DBus.Properties.Get",
         "string:org.mpris.MediaPlayer2.Player", "string:Metadata"],
        capture_output=True, text=True,
    ).stdout
    seen = False
    for line in out.splitlines():
        if key in line:
            seen = True
            continue
        if seen and 'string "' in line:
            return line.split('"')[1]
    return ""


def sanitize_now(text):
    """Make metadata safe for the NOW serial line and the device font."""
    text = text.encode("ascii", "ignore").decode()
    return text.replace(":", " ").strip()[:24]


def now_playing_linux():
    player = pick_player()
    if not player:
        return ("stop", "", "")
    status = mpris_prop(player, "PlaybackStatus")
    state = {"Playing": "play", "Paused": "pause"}.get(status, "stop")
    return (state, mpris_metadata(player, "xesam:title"),
            mpris_metadata(player, "xesam:artist"))


_winsdk_hint_shown = False


def now_playing_windows():
    """Track info via the Windows media-session API (winsdk package)."""
    global _winsdk_hint_shown
    try:
        import asyncio
        from winsdk.windows.media.control import (
            GlobalSystemMediaTransportControlsSessionManager as SessionManager,
        )
    except ImportError:
        if not _winsdk_hint_shown:
            _winsdk_hint_shown = True
            print("(pip install winsdk to enable the now-playing header)")
        return None

    async def query():
        mgr = await SessionManager.request_async()
        session = mgr.get_current_session()
        if not session:
            return ("stop", "", "")
        status = int(session.get_playback_info().playback_status)
        state = {4: "play", 5: "pause"}.get(status, "stop")  # 4=PLAYING 5=PAUSED
        props = await session.try_get_media_properties_async()
        return (state, props.title or "", props.artist or "")

    try:
        return asyncio.run(query())
    except Exception:
        return ("stop", "", "")


def now_playing():
    """(state, title, artist) for the header; state is play/pause/stop.

    None means "can't read track info here" (winsdk missing) — the bridge
    then simply never updates the header.
    """
    n = now_playing_windows() if IS_WINDOWS else now_playing_linux()
    if n is None:
        return None
    return (n[0], sanitize_now(n[1]), sanitize_now(n[2]))


def playback(action):
    """play_pause / next / previous."""
    if IS_WINDOWS:
        media_key(action)
        return
    if shutil.which("playerctl"):
        cmd = {"play_pause": "play-pause", "next": "next", "previous": "previous"}
        run_detached(f"playerctl {cmd[action]}")
        return
    method = {"play_pause": "PlayPause", "next": "Next", "previous": "Previous"}[action]
    player = pick_player()
    if not player:
        print("  (no controllable media player on MPRIS — is anything playing?)")
        return
    print(f"  -> {player}")
    run_detached(
        f"dbus-send --session --type=method_call --dest={player} "
        f"/org/mpris/MediaPlayer2 org.mpris.MediaPlayer2.Player.{method}"
    )


def volume(action):
    if IS_WINDOWS:
        media_key(action)
        return
    if shutil.which("wpctl"):
        cmd = {
            "volume_up": "wpctl set-volume -l 1.0 @DEFAULT_AUDIO_SINK@ 5%+",
            "volume_down": "wpctl set-volume @DEFAULT_AUDIO_SINK@ 5%-",
            "mute": "wpctl set-mute @DEFAULT_AUDIO_SINK@ toggle",
        }
    elif shutil.which("pactl"):
        cmd = {
            "volume_up": "pactl set-sink-volume @DEFAULT_SINK@ +5%",
            "volume_down": "pactl set-sink-volume @DEFAULT_SINK@ -5%",
            "mute": "pactl set-sink-mute @DEFAULT_SINK@ toggle",
        }
    else:
        cmd = {
            "volume_up": "amixer -q set Master 5%+",
            "volume_down": "amixer -q set Master 5%-",
            "mute": "amixer -q set Master toggle",
        }
    run_detached(cmd[action])


def handle(button_id, config):
    entry = config.get("buttons", {}).get(str(button_id))
    if not entry:
        print(f"  button {button_id}: no mapping")
        return
    kind = entry.get("type")
    if kind == "media":
        action = entry.get("action")
        print(f"  button {button_id}: media {action}")
        if action in ("volume_up", "volume_down", "mute"):
            volume(action)
        elif action in ("play_pause", "next", "previous"):
            playback(action)
        else:
            print(f"  unknown media action: {action}")
    elif kind == "launch":
        command = entry.get("command", "")
        print(f"  button {button_id}: launch {command}")
        run_detached(command)
    else:
        print(f"  unknown action type: {kind}")


def config_mtime():
    try:
        return CONFIG_PATH.stat().st_mtime
    except OSError:
        return 0


def session(ser, config):
    """Serve one serial connection until it errors out."""
    push_config(ser, config)
    mtime = config_mtime()
    pending = mtime
    last_check = time.time()
    now = None
    last_now = 0.0
    while True:
        line = ser.readline().decode(errors="replace").strip()

        # Push now-playing state to the header when it changes (~2 s poll).
        if time.time() - last_now >= 2:
            last_now = time.time()
            n = now_playing()
            if n is not None and n != now:
                now = n
                ser.write(f"NOW:{n[0]}:{n[1]}:{n[2]}\n".encode())

        # Hot-reload the config file when it changes (checked ~1/s, riding
        # on the 1s serial read timeout). Editors with autosave write partial
        # files mid-edit, so only reload once the mtime has been stable for a
        # full check cycle, and keep the old config if the file won't parse.
        if time.time() - last_check >= 1:
            last_check = time.time()
            m = config_mtime()
            if m != mtime and m == pending:
                mtime = m
                try:
                    fresh = load_config()
                except (json.JSONDecodeError, ValueError, OSError) as e:
                    print(f"Config reload failed (keeping old): {e}")
                else:
                    config.clear()
                    config.update(fresh)
                    print("Config changed, reloading + repushing layout")
                    push_config(ser, config)
            pending = m

        if not line:
            continue
        if line.startswith("BTN:"):
            try:
                handle(int(line[4:]), config)
            except ValueError:
                print(f"  bad button line: {line!r}")
        elif line.startswith("YELLOWDECK READY"):
            # Board (re)booted — its layout is back to defaults; re-push.
            print(f"[cyd] {line}")
            push_config(ser, config)
            now = None  # header is back to defaults too; re-send on next poll
        else:
            print(f"[cyd] {line}")


def main():
    while True:
        try:
            config = load_config()
            break
        except (json.JSONDecodeError, ValueError, OSError) as e:
            print(f"Config invalid ({e}) — fix {CONFIG_PATH}, retrying in 3s...")
            time.sleep(3)
    baud = config.get("baud", 115200)

    while True:
        port = find_port(config.get("port", "auto"))
        if not port:
            print("No serial port found, retrying in 3s...")
            time.sleep(3)
            continue
        try:
            with serial.Serial(port, baud, timeout=1) as ser:
                print(f"Listening on {port} @ {baud}")
                time.sleep(0.5)  # in case opening the port reset the board
                session(ser, config)
        except (serial.SerialException, OSError) as e:
            print(f"Serial error: {e}; reconnecting in 3s...")
            time.sleep(3)
        except KeyboardInterrupt:
            print("\nBye")
            sys.exit(0)


if __name__ == "__main__":
    main()
