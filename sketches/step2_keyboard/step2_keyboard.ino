// Step 2: type on a Bluetooth keyboard.
//
// First run: put the keyboard in pairing mode; the screen shows a passkey to type
// on it. After that the Inkplate reconnects on its own.
//
// Keys: Backspace (Option+Backspace deletes a word), Return, Cmd/Ctrl+L full
// refresh, Cmd/Ctrl+N clear the page, Cmd/Ctrl+Shift+K forget the keyboard.
// Serial logs Bluetooth events and refresh timings (tools/monitor.py).

#include <FastInkplate.h>
#include <Keyboard.h>
#include <Page.h>
#include <Text.h>
#include <fonts/Duo24.h>
#include <fonts/Duo40.h>

tw::FastInkplate display(INKPLATE_1BIT);
tw::Page page(display, Duo40, Duo40Ascent, Duo24, Duo24Ascent);
tw::Keyboard keyboard;

const uint32_t PASS_US = 20000;            // drive pass length chosen in Step 1
const uint32_t POWER_OFF_AFTER_MS = 1500;  // keep the panel powered between keystrokes
const uint32_t CLEAN_AFTER_IDLE_MS = 60000; // full refresh after this long without typing

std::string text;
bool dirty = false;
bool forceFull = true;
bool panelOn = false;
bool partialsSinceClean = false;
uint32_t lastKeyAt = 0;
uint32_t lastUpdateAt = 0;
uint32_t firstUnshownKeyAt = 0;
uint32_t seenRevision = UINT32_MAX;
bool showingPairing = false;

void deleteLastChar() {
    while (!text.empty()) {
        uint8_t last = text.back();
        text.pop_back();
        if ((last & 0xC0) != 0x80)
            break;
    }
}

void deleteLastWord() {
    while (!text.empty() && (text.back() == ' ' || text.back() == '\n'))
        text.pop_back();
    while (!text.empty() && text.back() != ' ' && text.back() != '\n')
        deleteLastChar();
}

void handle(const tw::KeyEvent &e, uint32_t now) {
    switch (e.type) {
    case tw::KeyEvent::Text:
        text += e.text;
        break;
    case tw::KeyEvent::Backspace:
        deleteLastChar();
        break;
    case tw::KeyEvent::BackspaceWord:
        deleteLastWord();
        break;
    case tw::KeyEvent::Enter:
        text += '\n';
        break;
    case tw::KeyEvent::Tab:
        text += "    ";
        break;
    case tw::KeyEvent::Escape:
        return;
    case tw::KeyEvent::Shortcut:
        if (e.key == 'l') {
            forceFull = true;
        } else if (e.key == 'n') {
            text.clear();
            page.reset();
            forceFull = true;
        } else if (e.key == 'k' && e.shift) {
            keyboard.forget();
        }
        return;
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

void keyboardStatus(char *out, size_t size) {
    switch (keyboard.state()) {
    case tw::Keyboard::State::Searching:
        snprintf(out, size, "Looking for keyboard…");
        break;
    case tw::Keyboard::State::Connecting:
        snprintf(out, size, "Connecting…");
        break;
    case tw::Keyboard::State::Pairing:
        snprintf(out, size, "Pairing…");
        break;
    case tw::Keyboard::State::Connected: {
        String name = keyboard.name();
        snprintf(out, size, "%s", name.length() ? name.c_str() : "Keyboard connected");
        break;
    }
    }
}

// Renders the writing page; returns true if the page turned.
bool drawPage() {
    char left[48], right[32];
    keyboardStatus(left, sizeof(left));
    snprintf(right, sizeof(right), "%d words", wordCount());
    return page.render(text, left, right);
}

void drawCentered(const GFXfont &font, int16_t ascent, int32_t y, const char *s) {
    size_t len = strlen(s);
    int32_t w = tw::measure(font, s, 0, len);
    tw::drawText(display, font, (display.width() - w) / 2, y + ascent, s, 0, len);
}

// Full-screen pairing instructions with the passkey.
void drawPairing() {
    char digits[16];
    snprintf(digits, sizeof(digits), "%06lu", (unsigned long)keyboard.passkey());
    char spaced[16] = {0};
    for (int i = 0, j = 0; i < 6; i++) {
        spaced[j++] = digits[i];
        if (i < 5)
            spaced[j++] = ' ';
    }
    display.clearDisplay();
    drawCentered(Duo40, Duo40Ascent, 180, "Type this code on the keyboard,");
    drawCentered(Duo40, Duo40Ascent, 240, "then press Return:");
    display.setTextSize(3); // the 40px font drawn 3× for the code
    int32_t w = tw::measure(Duo40, spaced, 0, strlen(spaced)) * 3;
    display.setFont(&Duo40);
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

void setup() {
    Serial.begin(115200);
    display.begin();
    display.setFullUpdateThreshold(0); // we decide when to do full refreshes
    keyboard.begin("Typewriter");
}

void loop() {
    uint32_t now = millis();
    tw::KeyEvent e;
    while (keyboard.poll(e))
        handle(e, now);

    // Keyboard state changes: pairing screen, or status bar update.
    uint32_t rev = keyboard.revision();
    if (rev != seenRevision) {
        seenRevision = rev;
        bool pairing = keyboard.state() == tw::Keyboard::State::Pairing;
        if (pairing) {
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
            dirty = true;
            if (!firstUnshownKeyAt)
                firstUnshownKeyAt = now;
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
            display.quickPartialUpdate(true, PASS_US);
            panelOn = true;
            partialsSinceClean = true;
            Serial.printf("partial: draw %lu ms, panel %lu ms, oldest key waited %lu ms\n", (unsigned long)(t1 - t0),
                          (unsigned long)(millis() - t1), (unsigned long)(millis() - firstUnshownKeyAt));
        }
        lastUpdateAt = millis();
        firstUnshownKeyAt = 0;
        return;
    }

    if (panelOn && now - lastUpdateAt > POWER_OFF_AFTER_MS) {
        display.einkOff();
        panelOn = false;
    }

    if (partialsSinceClean && now - lastKeyAt > CLEAN_AFTER_IDLE_MS && now - lastUpdateAt > CLEAN_AFTER_IDLE_MS)
        fullRefresh("idle");
}
