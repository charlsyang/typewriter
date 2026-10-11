# Typewriter

Firmware that turns a Soldered Inkplate 5 Gen2 into a distraction-free typewriter:
Bluetooth keyboard in, plain text on e-ink, files synced to Dropbox.

Plan and progress: https://claude.ai/code/artifact/c73d23b6-ee64-4214-970b-06e358a6e9f3

## Setup (macOS, Apple Silicon)

```bash
brew install arduino-cli
arduino-cli config init
arduino-cli config add board_manager.additional_urls https://github.com/SolderedElectronics/Inkplate-Board-Definitions-for-Arduino-IDE/raw/refs/heads/main/package_Inkplate_Boards_index.json
arduino-cli core update-index
arduino-cli core install soldered-inkplate-boards:esp32
arduino-cli lib install InkplateLibrary
```

The `ctags` that arduino-cli downloads is Intel-only and needs Rosetta. Without Rosetta,
build Arduino's fork natively and drop it in place of
`~/Library/Arduino15/packages/builtin/tools/ctags/5.8-arduino11/ctags`:

```bash
git clone --depth 1 https://github.com/arduino/ctags.git && cd ctags
grep -rl __unused__ . | grep -E '\.(c|h)$' | xargs sed -i '' 's/__unused__/CTAGS_UNUSED/g'
./configure && make
```

## Build, upload, watch serial

Each sketch has a `sketch.yaml` with the board and port.

```bash
cd sketches/step0_hello   # sketches using lib/Typewriter also need: --library ../../lib/Typewriter
arduino-cli compile --build-path ../../build/step0 .
arduino-cli upload --build-path ../../build/step0 .
../../tools/monitor.py
```

## Board facts (measured)

ESP32-D0WD-V3 at 240 MHz, 4 MB flash, 4 MB PSRAM. Rotation 0 is landscape, 1280×720.

## Fonts

Body and status fonts are iA Writer Duo (variable, weight 400), rasterized to 1-bit with a
black/white cutoff of 64 ("darker edges"):

```bash
tools/.venv/bin/python tools/make_font.py Duo40 40 --weight 400 --cutoff 64
tools/.venv/bin/python tools/make_font.py Duo24 24 --weight 400 --cutoff 64 --line-spacing 1.2
```

`tools/preview_fonts.py` renders the writing screen at 1280×720 for comparing options.

The sleep screen's highlights use Libron (v0.31, SIL OFL, `fonts/Libron-*.ttf`), kept
anti-aliased for the panel's 3-bit mode, with its kerning and ligatures (`uharfbuzz` in the
venv):

```bash
cd tools
../tools/.venv/bin/python make_gray_font.py Libron52 52 ../fonts/Libron-Regular.ttf
../tools/.venv/bin/python make_gray_font.py Libron44 44 ../fonts/Libron-Regular.ttf
../tools/.venv/bin/python make_gray_font.py Libron36 36 ../fonts/Libron-Regular.ttf
../tools/.venv/bin/python make_gray_font.py Libron28 28 ../fonts/Libron-Regular.ttf
../tools/.venv/bin/python make_gray_font.py LibronItalic28 28 ../fonts/Libron-Italic.ttf
```
