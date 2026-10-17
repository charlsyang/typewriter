# Sketches

The early experiments the firmware grew from: `step0_hello` (display test),
`step1_font_preview`, `step1_typing` (partial-update timing) and `step2_keyboard`
(Bluetooth keyboard). They build with arduino-cli and Soldered's board package, not
PlatformIO. Each `sketch.yaml` names the board and the serial port.

```bash
brew install arduino-cli
arduino-cli config init
arduino-cli config add board_manager.additional_urls https://github.com/SolderedElectronics/Inkplate-Board-Definitions-for-Arduino-IDE/raw/refs/heads/main/package_Inkplate_Boards_index.json
arduino-cli core update-index
arduino-cli core install soldered-inkplate-boards:esp32
arduino-cli lib install InkplateLibrary
```

Build and upload from a sketch's folder. Sketches that use `lib/Typewriter` also need
`--library ../../lib/Typewriter`:

```bash
cd sketches/step0_hello
arduino-cli compile --build-path ../../build/step0 .
arduino-cli upload --build-path ../../build/step0 .
../../tools/monitor.py
```

On Apple Silicon without Rosetta, the `ctags` that arduino-cli downloads (Intel-only) won't
run. Build Arduino's fork natively and put it in place of
`~/Library/Arduino15/packages/builtin/tools/ctags/5.8-arduino11/ctags`:

```bash
git clone --depth 1 https://github.com/arduino/ctags.git && cd ctags
grep -rl __unused__ . | grep -E '\.(c|h)$' | xargs sed -i '' 's/__unused__/CTAGS_UNUSED/g'
./configure && make
```
