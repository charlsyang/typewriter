# Typewriter

Firmware that turns a Soldered Inkplate 5 Gen2 into a distraction-free typewriter. You type on
a Bluetooth keyboard and plain text shows up on e-ink. There's no browser, no notifications and
no menus beyond a file list.

- **Daily notes.** Each day opens its own note, `YYYY-MM-DD.md` (the Obsidian daily-note name).
  It autosaves to flash and syncs with a Dropbox folder over Wi-Fi.
- **Drafts.** Cmd+O lists your essay drafts from a second Dropbox folder (`.md`/`.mdx` files
  without `isPublished: true` in their front matter). They're kept on the device and sync the
  same way. Cmd+N starts a new one.
- **Sleep screen.** After 10 minutes idle, or on the WAKE button, the device saves, syncs and
  deep-sleeps. If a microSD card holds your Readwise library, the sleep screen shows one of your
  highlights.

Keys: Backspace (Option+Backspace deletes a word), Return, Cmd+Z / Cmd+Shift+Z undo and redo,
Cmd+S save and sync, Cmd+I status, Cmd+L full refresh, Cmd+D today's note, ↑/↓ read back
through earlier pages, Cmd+Shift+K forget the keyboard. Ctrl works in place of Cmd. The header
comment in [`firmware/typewriter/typewriter.ino`](firmware/typewriter/typewriter.ino) has the
full list.

## Hardware

- **Soldered Inkplate 5 Gen2**: ESP32-D0WD-V3 at 240 MHz, 4 MB flash, 4 MB PSRAM, 1280×720
  e-ink panel (rotation 0 is landscape), with an RTC and a LiPo battery connector.
- **A Bluetooth LE keyboard.** The board pairs with it using a passkey shown on screen,
  then reconnects on its own. Classic Bluetooth keyboards (non-LE) won't work. The layout is
  US, with the Mac Option-key punctuation (– — “ ” ‘ ’ …).
- **A microSD card** (optional), for the Readwise highlights.

## Setup

You'll need [PlatformIO](https://platformio.org) (the CLI is enough) and Python 3 for the
helper scripts in `tools/`. Tested on macOS.

### 1. Secrets

```bash
cp firmware/typewriter/secrets.example.h firmware/typewriter/secrets.h
```

Fill in `WIFI_SSID` and `WIFI_PASSWORD`. `secrets.h` is gitignored.

For Dropbox, create an app at [dropbox.com/developers/apps](https://www.dropbox.com/developers/apps)
with "Full Dropbox" access and the permissions `files.metadata.read`, `files.content.read` and
`files.content.write`. Then run this with the app's key:

```bash
tools/dropbox_auth.py YOUR_APP_KEY
```

The script opens your browser to approve the app, then writes `DROPBOX_APP_KEY` and a
long-lived `DROPBOX_REFRESH_TOKEN` into `secrets.h`. It never prints the token.

The Dropbox folders default to `/OS/Daily Notes/` for daily notes and `/Site/blog` for drafts.
To use other folders, set `DROPBOX_DAILY_NOTES` and `DROPBOX_ESSAYS` in `secrets.h`; see
`secrets.example.h`.

### 2. Build and flash

```bash
pio run -e inkplate5v2 -t upload
pio device monitor
```

`platformio.ini` uses the [pioarduino](https://github.com/pioarduino/platform-espressif32)
platform, which rebuilds the ESP-IDF libraries from `custom_sdkconfig`. That's what makes room
for Bluetooth and Wi-Fi together; the comment at the top of the file explains the details.
The Inkplate library is pulled from Soldered's GitHub at a pinned commit (11.2.0). The first
build takes several minutes. If it stops with `'LittleFS' was not declared`, run it again.
Later builds are quick.

Before flashing, set `upload_port` and `monitor_port` in `platformio.ini` to your board's
serial port (`pio device list`).

### 3. First run

1. Put the keyboard in pairing mode and type the passkey shown on screen.
2. The clock comes from the network once Wi-Fi connects, but the time zone starts as UTC. To
   set your local zone, connect over USB and run `tools/settime.py`. It sends the Mac's zone,
   daylight saving rules included.

### Tests

The sync logic, undo history and highlight code build on the host against in-memory fakes:

```bash
test/run.sh
```

## Readwise highlights (sleep screen)

Put your token from [readwise.io/access_token](https://readwise.io/access_token) in
`secrets.h` as `READWISE_TOKEN`. Then copy your whole library onto the SD card once, over USB
(about 90 s per MB):

```bash
tools/readwise_seed.py --send
```

After that the device fetches only what changed, about once a month over Wi-Fi. To sync now,
send Ctrl+R `hlsync` over USB. `tools/highlight_sizes.py` shows how your library fits the
sleep screen's text sizes.

## USB commands

With the board on USB, these scripts talk to it (default port `/dev/cu.usbserial-110`; pass
another as an argument):

- `tools/log.py` prints the serial log without resetting the board. `tools/monitor.py` resets
  it first.
- `tools/type.py` types from your terminal.
- `tools/settime.py` sets the clock and time zone.
- `tools/sd_put.py` copies files onto the SD card.

Ctrl+R followed by a line runs a command: `status`, `ls`, `sd`, `rm <path>`, `sleep [seconds]`,
`hl` (check the highlight files), `hlsync`, `hlpick [n]`, `hltest [size] [index]` and `hltour`.

## Fonts

Body and status text use iA Writer Duo (variable, weight 400), rasterized to 1-bit with a
black/white cutoff of 64. The sleep screen's highlights use Libron (v0.31), kept anti-aliased
for the panel's 3-bit mode, with its kerning and ligatures. The generated headers are in
`lib/Typewriter/src/fonts/`. To regenerate them, use a venv with `pillow` and `uharfbuzz`:

```bash
tools/.venv/bin/python tools/make_font.py Duo40 40 --weight 400 --cutoff 64
tools/.venv/bin/python tools/make_font.py Duo24 24 --weight 400 --cutoff 64 --line-spacing 1.2
cd tools && ../tools/.venv/bin/python make_gray_font.py Libron52 52 ../fonts/Libron-Regular.ttf
```

`tools/preview_fonts.py` renders the writing screen at 1280×720 so you can compare options.

## Repository layout

- `firmware/typewriter/`: the main sketch and `secrets.h`
- `lib/Typewriter/`: display, keyboard, storage, Dropbox, Readwise and sync code
- `variants/`, `huge_app.csv`: board pins and partition table
- `tools/`: setup, USB and font scripts
- `test/`: host tests
- `sketches/`: the early step-by-step experiments, built with arduino-cli (see
  [`sketches/README.md`](sketches/README.md))

## License

The code is under the [MIT License](LICENSE). The fonts in `fonts/` and the headers generated
from them in `lib/Typewriter/src/fonts/` stay under the SIL Open Font License 1.1. See
[`fonts/iA-Fonts-LICENSE.md`](fonts/iA-Fonts-LICENSE.md) and
[`fonts/Libron-LICENSE.txt`](fonts/Libron-LICENSE.txt).
