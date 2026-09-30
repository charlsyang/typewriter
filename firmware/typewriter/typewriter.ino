// Typewriter: write into today's daily note on a Bluetooth keyboard.
//
// The note lives in flash at /daily/YYYY-MM-DD.md (the Obsidian daily note name)
// and autosaves a couple of seconds after you stop typing. The screen shows only
// your text; a footer line appears when something needs attention, or on Cmd+I.
//
// Keys: Backspace (Option+Backspace deletes a word), Return, Cmd+S save now,
// Cmd+I show status, Cmd+L full refresh, Cmd+Shift+K forget the keyboard.
// Ctrl works in place of Cmd.
//
// Over USB (tools/settime.py, tools/type.py): Ctrl+T sets the clock, Ctrl+K
// forgets the keyboard, other bytes type text. Serial logs events and timings.

#include <Clock.h>
#include <FastInkplate.h>
#include <Keyboard.h>
#include <Page.h>
#include <Storage.h>
#include <Text.h>
#include <fonts/Duo24.h>
#include <fonts/Duo40.h>

tw::FastInkplate display(INKPLATE_1BIT);
tw::Page page(display, Duo40, Duo40Ascent, Duo24, Duo24Ascent);
tw::Keyboard keyboard;

const uint32_t PASS_US = 20000;               // drive pass length chosen in Step 1
const uint32_t FOOTER_PASS_US = 55000;        // library-length passes: erases notices cleanly
const uint32_t POWER_OFF_AFTER_MS = 1500;     // keep the panel powered between keystrokes
const uint32_t CLEAN_AFTER_IDLE_MS = 60000;   // full refresh after this long without typing
const uint32_t SAVE_AFTER_IDLE_MS = 2000;     // autosave once typing pauses this long...
const uint32_t SAVE_AT_LEAST_EVERY_MS = 30000; // ...or at least this often while typing
const uint32_t BATTERY_CHECK_EVERY_MS = 60000;
const int LOW_BATTERY_PERCENT = 15;

struct Document {
    std::string path; // in flash, e.g. /daily/2026-09-29.md
    std::string name; // shown to you, e.g. 2026-09-29.md
    std::string text;
    bool unsaved = false;
    bool saveFailed = false;
    uint32_t firstUnsavedAt = 0;
};

Document doc;
uint32_t lastKeyAt = 0;
bool dirty = false;
bool forceFull = true;
bool showStatus = false; // Cmd+I: status line until the next key
bool panelOn = false;
bool partialsSinceClean = false;
bool showingPairing = false;
uint32_t lastUpdateAt = 0;
uint32_t firstUnshownKeyAt = 0;
uint32_t seenKeyboardRevision = UINT32_MAX;
int batteryPercent = -1; // -1: no battery reading
uint32_t lastBatteryCheckAt = 0;

// ---- Document ----

void openDocument(const char *path, const char *name) {
    doc = Document();
    doc.path = path;
    doc.name = name;
    if (tw::storage::exists(path) && !tw::storage::read(path, doc.text))
        Serial.printf("open: couldn't read %s\n", path);
    page.showEnd(doc.text);
    forceFull = true;
    Serial.printf("open: %s (%u bytes)\n", path, (unsigned)doc.text.size());
}

void openToday() {
    char date[11];
    if (tw::wallclock::isSet()) {
        tw::wallclock::today(date);
    } else {
        strcpy(date, "undated");
    }
    std::string name = std::string(date) + ".md";
    openDocument(("/daily/" + name).c_str(), name.c_str());
}

void save(const char *why) {
    if (!doc.unsaved)
        return;
    uint32_t t0 = millis();
    bool ok = tw::storage::write(doc.path.c_str(), doc.text);
    doc.saveFailed = !ok;
    doc.unsaved = !ok;
    if (ok)
        doc.firstUnsavedAt = 0;
    Serial.printf("save (%s): %s, %u bytes, %lu ms\n", why, ok ? "ok" : "FAILED", (unsigned)doc.text.size(),
                  (unsigned long)(millis() - t0));
    if (!ok)
        dirty = true; // show the notice
}

void edited(uint32_t now) {
    if (!doc.unsaved)
        doc.firstUnsavedAt = now;
    doc.unsaved = true;
}

void deleteLastChar() {
    while (!doc.text.empty()) {
        uint8_t last = doc.text.back();
        doc.text.pop_back();
        if ((last & 0xC0) != 0x80)
            break;
    }
}

void deleteLastWord() {
    std::string &t = doc.text;
    while (!t.empty() && (t.back() == ' ' || t.back() == '\n'))
        t.pop_back();
    while (!t.empty() && t.back() != ' ' && t.back() != '\n')
        deleteLastChar();
}

int wordCount() {
    int words = 0;
    bool inWord = false;
    for (char c : doc.text) {
        bool space = c == ' ' || c == '\n';
        if (!space && !inWord)
            words++;
        inWord = !space;
    }
    return words;
}

// ---- Input ----

void markChanged(uint32_t now) {
    if (!dirty)
        firstUnshownKeyAt = now;
    dirty = true;
}

void handleKey(const tw::KeyEvent &e, uint32_t now) {
    lastKeyAt = now;
    if (showStatus) {
        showStatus = false;
        markChanged(now);
    }
    switch (e.type) {
    case tw::KeyEvent::Text:
        doc.text += e.text;
        break;
    case tw::KeyEvent::Backspace:
        deleteLastChar();
        break;
    case tw::KeyEvent::BackspaceWord:
        deleteLastWord();
        break;
    case tw::KeyEvent::Enter:
        doc.text += '\n';
        break;
    case tw::KeyEvent::Tab:
        doc.text += "    ";
        break;
    case tw::KeyEvent::Escape:
        return;
    case tw::KeyEvent::Shortcut:
        if (e.key == 's') {
            save("Cmd+S");
            markChanged(now);
        } else if (e.key == 'i') {
            showStatus = true;
            markChanged(now);
        } else if (e.key == 'l') {
            forceFull = true;
        } else if (e.key == 'k' && e.shift) {
            keyboard.forget();
        }
        return;
    }
    edited(now);
    markChanged(now);
}

// Ctrl+T over USB: "<utc epoch> <posix tz>\n" sets the clock (tools/settime.py).
// Any other byte types text, like tools/type.py.
void handleSerial(uint32_t now) {
    static std::string pendingUtf8;
    while (Serial.available()) {
        int b = Serial.read();
        if (b == 0x14) {
            String line = Serial.readStringUntil('\n');
            int space = line.indexOf(' ');
            uint32_t epoch = line.substring(0, space).toInt();
            String tz = space > 0 ? line.substring(space + 1) : "UTC0";
            tz.trim();
            if (epoch > 1700000000) {
                save("clock change");
                tw::wallclock::set(display, epoch, tz.c_str());
                Serial.printf("clock set: %lu %s\n", (unsigned long)epoch, tz.c_str());
                openToday();
            }
            continue;
        }
        if (b == 0x0B) { // Ctrl+K over USB: forget the keyboard
            Serial.println("kbd: forgetting paired keyboards");
            keyboard.forget();
            continue;
        }
        tw::KeyEvent e = {};
        if (b == 0x7F || b == 0x08) {
            e.type = tw::KeyEvent::Backspace;
        } else if (b == '\r' || b == '\n') {
            e.type = tw::KeyEvent::Enter;
        } else if (b >= 0x20) {
            // Collect whole UTF-8 characters before typing them.
            pendingUtf8 += (char)b;
            uint8_t lead = pendingUtf8[0];
            size_t need = lead < 0x80 ? 1 : (lead & 0xE0) == 0xC0 ? 2 : (lead & 0xF0) == 0xE0 ? 3 : 4;
            if (pendingUtf8.size() < need)
                continue;
            e.type = tw::KeyEvent::Text;
            strlcpy(e.text, pendingUtf8.c_str(), sizeof(e.text));
            pendingUtf8.clear();
        } else {
            continue;
        }
        handleKey(e, now);
    }
}

// ---- Screen ----

void checkBattery(uint32_t now) {
    if (lastBatteryCheckAt && now - lastBatteryCheckAt < BATTERY_CHECK_EVERY_MS)
        return;
    lastBatteryCheckAt = now;
    double v = display.readBattery();
    // A Li-ion cell reads about 3.4 V empty to 4.15 V full. On USB with no
    // battery the charger's own 4.2 V shows through, so treat that as no battery;
    // a real cell only reaches it while fully charged on USB.
    int previous = batteryPercent;
    if (v < 3.0 || v >= 4.18)
        batteryPercent = -1;
    else
        batteryPercent = constrain((int)((v - 3.4) / (4.15 - 3.4) * 100), 0, 100);
    Serial.printf("battery: %.2f V (%d%%)\n", v, batteryPercent);
    bool wasLow = previous >= 0 && previous <= LOW_BATTERY_PERCENT;
    bool isLow = batteryPercent >= 0 && batteryPercent <= LOW_BATTERY_PERCENT;
    if (wasLow != isLow)
        markChanged(now);
}

// The footer: problems first, then the Cmd+I status. Empty when all is well.
void footer(char *out, size_t size) {
    out[0] = 0;
    tw::Keyboard::State kb = keyboard.state();
    if (doc.saveFailed) {
        snprintf(out, size, "Not saved: couldn’t write to flash. Cmd+S to retry.");
    } else if (kb == tw::Keyboard::State::Searching || kb == tw::Keyboard::State::Connecting) {
        snprintf(out, size, "Keyboard not connected. Press a key on it to wake it.");
    } else if (!tw::wallclock::isSet()) {
        snprintf(out, size, "Clock not set, so this is undated.md. Run tools/settime.py over USB.");
    } else if (batteryPercent >= 0 && batteryPercent <= LOW_BATTERY_PERCENT) {
        snprintf(out, size, "Battery low: %d%%", batteryPercent);
    } else if (showStatus) {
        char battery[24] = "USB power";
        if (batteryPercent >= 0)
            snprintf(battery, sizeof(battery), "battery %d%%", batteryPercent);
        snprintf(out, size, "%s · %d words · %s · %s", doc.name.c_str(), wordCount(),
                 doc.unsaved ? "not saved yet" : "saved", battery);
    }
}

std::string shownFooter;
bool footerChanged = false; // since the last render

// Renders the writing page; returns true if the page turned.
bool drawPage() {
    char text[128];
    footer(text, sizeof(text));
    footerChanged = shownFooter != text;
    shownFooter = text;
    return page.render(doc.text, text);
}

void drawCentered(int32_t y, const char *s) {
    size_t len = strlen(s);
    int32_t w = tw::measure(Duo40, s, 0, len);
    tw::drawText(display, Duo40, (display.width() - w) / 2, y + Duo40Ascent, s, 0, len);
}

// Full-screen pairing instructions with the passkey.
void drawPairing() {
    char digits[8], spaced[16] = {0};
    snprintf(digits, sizeof(digits), "%06lu", (unsigned long)keyboard.passkey());
    for (int i = 0, j = 0; i < 6; i++) {
        spaced[j++] = digits[i];
        if (i < 5)
            spaced[j++] = ' ';
    }
    display.clearDisplay();
    drawCentered(180, "Type this code on the keyboard,");
    drawCentered(240, "then press Return:");
    display.setFont(&Duo40);
    display.setTextSize(3);
    int32_t w = tw::measure(Duo40, spaced, 0, strlen(spaced)) * 3;
    display.setCursor((display.width() - w) / 2, 330 + Duo40Ascent * 3);
    display.print(spaced);
    display.setTextSize(1);
}

void fullRefresh(const char *why) {
    uint32_t t0 = millis();
    partialsSinceClean = false;
    drawPage();
    display.display();
    panelOn = false;
    lastUpdateAt = millis();
    Serial.printf("full refresh (%s): %lu ms\n", why, (unsigned long)(lastUpdateAt - t0));
}

// ---- Main ----

void setup() {
    Serial.begin(115200);
    display.begin();
    display.setFullUpdateThreshold(0); // we decide when to do full refreshes
    if (!tw::storage::begin())
        Serial.println("storage: mount FAILED");
    Serial.printf("storage: %u of %u KB used\n", (unsigned)(tw::storage::usedBytes() / 1024),
                  (unsigned)(tw::storage::totalBytes() / 1024));
    tw::wallclock::begin(display);
    openToday();
    keyboard.begin("Typewriter");
}

void loop() {
    uint32_t now = millis();
    handleSerial(now);
    tw::KeyEvent e;
    while (keyboard.poll(e))
        handleKey(e, now);

    // Autosave: after a pause, or regularly during long stretches of typing.
    if (doc.unsaved && !doc.saveFailed &&
        (now - lastKeyAt > SAVE_AFTER_IDLE_MS || now - doc.firstUnsavedAt > SAVE_AT_LEAST_EVERY_MS))
        save("auto");

    checkBattery(now);

    // Keyboard state changes: the pairing screen, or the footer notice.
    uint32_t rev = keyboard.revision();
    if (rev != seenKeyboardRevision) {
        seenKeyboardRevision = rev;
        if (keyboard.state() == tw::Keyboard::State::Pairing) {
            drawPairing();
            display.display();
            panelOn = false;
            showingPairing = true;
            return;
        }
        if (showingPairing) {
            showingPairing = false;
            forceFull = true;
        } else {
            markChanged(now);
        }
    }
    if (showingPairing)
        return;

    if (forceFull) {
        forceFull = false;
        dirty = false;
        fullRefresh("requested");
        return;
    }

    if (dirty) {
        dirty = false;
        uint32_t t0 = millis();
        if (drawPage()) {
            display.display();
            partialsSinceClean = false;
            panelOn = false;
            Serial.printf("page turn: %lu ms\n", (unsigned long)(millis() - t0));
        } else {
            uint32_t t1 = millis();
            // Quick passes leave a faint trace when erasing, which is fine for a
            // typo but not for a notice that should be gone.
            display.quickPartialUpdate(true, footerChanged ? FOOTER_PASS_US : PASS_US);
            panelOn = true;
            partialsSinceClean = true;
            Serial.printf("partial: draw %lu ms, panel %lu ms, key waited %lu ms\n", (unsigned long)(t1 - t0),
                          (unsigned long)(millis() - t1), (unsigned long)(millis() - firstUnshownKeyAt));
        }
        lastUpdateAt = millis();
        return;
    }

    if (panelOn && now - lastUpdateAt > POWER_OFF_AFTER_MS) {
        display.einkOff();
        panelOn = false;
    }

    if (partialsSinceClean && now - lastKeyAt > CLEAN_AFTER_IDLE_MS && now - lastUpdateAt > CLEAN_AFTER_IDLE_MS)
        fullRefresh("idle");
}
