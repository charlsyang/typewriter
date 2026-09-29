// Step 1: type from the laptop over USB (tools/type.py) to tune how the screen
// refreshes while writing, before the Bluetooth keyboard exists.
//
// Keys that arrive while the panel is busy are batched into the next partial
// update. When the page fills it turns with one full refresh, and a long pause
// cleans up ghosting with a full refresh. Timings go to serial.
//
// Controls (from tools/type.py): Backspace deletes, Enter starts a new line,
// Ctrl+L forces a full refresh, Ctrl+N clears the page, Ctrl+F switches between
// the quick partial update and the library's own (to compare).

#include <FastInkplate.h>
#include <Page.h>
#include <fonts/Duo24.h>
#include <fonts/Duo40.h>

tw::FastInkplate display(INKPLATE_1BIT);
tw::Page page(display, Duo40, Duo40Ascent, Duo24, Duo24Ascent);

// Refresh policy under test.
const uint32_t POWER_OFF_AFTER_MS = 1500;  // keep the panel powered between keystrokes
const uint32_t CLEAN_AFTER_IDLE_MS = 60000; // full refresh once typing pauses this long...
const uint16_t CLEAN_MIN_PARTIALS = 1;      // ...if anything changed since the last one

std::string text;
bool dirty = false;
bool forceFull = true;
bool panelOn = false;
bool quick = true;      // quick partial update vs the library's
bool inEscape = false;  // skipping a terminal escape sequence (arrow keys etc.)
int utf8Pending = 0;    // continuation bytes still expected for the last character
uint16_t partialsSinceClean = 0;
uint32_t lastKeyAt = 0;
uint32_t lastUpdateAt = 0;
uint32_t firstUnshownKeyAt = 0;

void deleteLastChar() {
    while (!text.empty()) {
        uint8_t last = text.back();
        text.pop_back();
        if ((last & 0xC0) != 0x80)
            break;
    }
}

void handleByte(uint8_t b, uint32_t now) {
    if (inEscape) {
        // CSI sequences end with a letter or '~'; a lone ESC ends at the next byte.
        if ((b >= 'A' && b <= 'Z') || (b >= 'a' && b <= 'z') || b == '~')
            inEscape = false;
        return;
    }
    if (utf8Pending > 0) {
        text.push_back(b);
        utf8Pending--;
    } else if (b == 0x1B) {
        inEscape = true;
        return;
    } else if (b == 0x7F || b == 0x08) {
        deleteLastChar();
    } else if (b == '\r' || b == '\n') {
        text.push_back('\n');
    } else if (b == 0x0C) { // Ctrl+L
        forceFull = true;
        return;
    } else if (b == 0x06) { // Ctrl+F
        quick = !quick;
        forceFull = true;
        return;
    } else if (b == 0x0E) { // Ctrl+N
        text.clear();
        page.reset();
        forceFull = true;
        return;
    } else if (b >= 0x20) {
        text.push_back(b);
        if ((b & 0xE0) == 0xC0)
            utf8Pending = 1;
        else if ((b & 0xF0) == 0xE0)
            utf8Pending = 2;
        else if ((b & 0xF8) == 0xF0)
            utf8Pending = 3;
    } else {
        return; // other control keys do nothing
    }
    if (!dirty)
        firstUnshownKeyAt = now;
    dirty = true;
    lastKeyAt = now;
}

int wordCount() {
    int words = 0;
    bool inWord = false;
    for (char c : text) {
        bool space = c == ' ' || c == '\n';
        if (!space && !inWord)
            words++;
        inWord = !space;
    }
    return words;
}

// Renders the page; returns true if the page turned.
bool draw() {
    char left[48], right[48];
    snprintf(left, sizeof(left), "Typing test · %s refresh", quick ? "quick" : "library");
    snprintf(right, sizeof(right), "%d words", wordCount());
    return page.render(text, left, right);
}

void fullRefresh(const char *why) {
    uint32_t t0 = millis();
    partialsSinceClean = 0;
    draw();
    display.display();
    panelOn = false;
    lastUpdateAt = millis();
    Serial.printf("full refresh (%s): %lu ms\n", why, (unsigned long)(lastUpdateAt - t0));
}

void setup() {
    Serial.begin(115200);
    display.begin();
    display.setFullUpdateThreshold(0); // we decide when to do full refreshes
}

void loop() {
    uint32_t now = millis();
    while (Serial.available())
        handleByte(Serial.read(), now);

    if (forceFull) {
        forceFull = false;
        dirty = false;
        fullRefresh("requested");
        return;
    }

    if (dirty && utf8Pending == 0) {
        dirty = false;
        uint32_t t0 = millis();
        if (draw()) {
            display.display();
            partialsSinceClean = 0;
            panelOn = false;
            Serial.printf("page turn: %lu ms\n", (unsigned long)(millis() - t0));
        } else {
            uint32_t t1 = millis();
            uint32_t rows = quick ? display.quickPartialUpdate() : display.partialUpdate(false, true);
            panelOn = true;
            partialsSinceClean++;
            Serial.printf("%s partial: draw %lu ms, panel %lu ms, oldest key waited %lu ms, rows %lu\n",
                          quick ? "quick" : "library", (unsigned long)(t1 - t0), (unsigned long)(millis() - t1),
                          (unsigned long)(millis() - firstUnshownKeyAt), (unsigned long)rows);
        }
        lastUpdateAt = millis();
        return;
    }

    if (panelOn && now - lastUpdateAt > POWER_OFF_AFTER_MS) {
        display.einkOff();
        panelOn = false;
    }

    if (!dirty && partialsSinceClean >= CLEAN_MIN_PARTIALS && now - lastKeyAt > CLEAN_AFTER_IDLE_MS)
        fullRefresh("pause");
}
