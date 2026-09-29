// Step 1: a scripted typing session to tune how the screen refreshes while writing.
//
// Characters "arrive" on a human-like schedule (with pauses and a few typos fixed by
// backspace). The loop redraws whenever text changed and the panel is free, so fast
// typing is batched into one partial update. Timings go to serial (tools/monitor.py).

#include <Inkplate.h>
#include <Page.h>
#include <fonts/Duo24.h>
#include <fonts/Duo40.h>

Inkplate display(INKPLATE_1BIT);
tw::Page page(display, Duo40, Duo40Ascent, Duo24, Duo24Ascent);

// Refresh policy under test.
const uint32_t POWER_OFF_AFTER_MS = 1500; // keep the panel powered between keystrokes
const uint32_t CLEAN_AFTER_IDLE_MS = 4000; // full refresh when typing pauses this long...
const uint16_t CLEAN_MIN_PARTIALS = 30;    // ...and at least this many partials happened

// '\b' is a backspace; '~' is a 1.5 s thinking pause.
const char *SCRIPT = "The page moves only one way. There is no going back to fix the sentence above, so the "
                     "next one has to carry it forward instead.~\n\n"
                     "“Write first, edit latr\b\b\ber,” she said—and meant it. It’s strange how much quieter "
                     "the mind gets when the delete key only reaches a few letters…~\n\n"
                     "Tomorrow the draft will look diffrent\b\b\b\b\berent. Today it only has to exist, one line "
                     "at a time, until the page is full and the next one begins. Keep going. Don’t look back."
                     "~ The words that feel wrong now are the ones that get you to the words that feel right.";

std::string text;
size_t scriptPos = 0;
uint32_t nextKeyAt = 0;
bool dirty = true;
bool panelOn = false;
uint16_t partialsSinceClean = 0;
uint32_t lastChangeAt = 0;
uint32_t lastUpdateAt = 0;
uint32_t updates = 0;

// Milliseconds until the next simulated key: ~65 wpm with some jitter.
uint32_t keyDelay(char c) {
    if (c == '~')
        return 1500;
    uint32_t d = 110 + random(0, 90);
    if (c == ' ')
        d += 40;
    if (c == '.' || c == ',')
        d += 250;
    if (c == '\b')
        d = 140;
    return d;
}

// Feeds every key whose time has come; returns how many changed the text.
int feedKeys(uint32_t now) {
    int n = 0;
    while (SCRIPT[scriptPos] && now >= nextKeyAt) {
        char c = SCRIPT[scriptPos++];
        if (c == '\b') {
            // Remove one UTF-8 character.
            while (!text.empty()) {
                uint8_t last = text.back();
                text.pop_back();
                if ((last & 0xC0) != 0x80)
                    break;
            }
            n++;
        } else if (c != '~') {
            text.push_back(c);
            n++;
        }
        nextKeyAt += keyDelay(c);
    }
    return n;
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

void draw() {
    char left[48], right[48];
    snprintf(left, sizeof(left), "Typing test · update %lu", (unsigned long)updates);
    snprintf(right, sizeof(right), "%d words", wordCount());
    page.render(text, left, right);
}

void setup() {
    Serial.begin(115200);
    display.begin();
    display.setFullUpdateThreshold(0); // we decide when to do full refreshes
    randomSeed(42);
    draw();
    display.display();
    delay(1000);
    nextKeyAt = millis();
}

void loop() {
    uint32_t now = millis();
    int typed = feedKeys(now);
    if (typed) {
        dirty = true;
        lastChangeAt = now;
    }

    if (dirty) {
        uint32_t t0 = millis();
        draw();
        uint32_t t1 = millis();
        display.partialUpdate(false, true);
        uint32_t t2 = millis();
        panelOn = true;
        dirty = false;
        partialsSinceClean++;
        updates++;
        lastUpdateAt = t2;
        Serial.printf("update %lu: +%d keys, draw %lu ms, panel %lu ms, key-to-screen %lu ms\n",
                      (unsigned long)updates, typed, (unsigned long)(t1 - t0), (unsigned long)(t2 - t1),
                      (unsigned long)(t2 - lastChangeAt));
        return;
    }

    if (panelOn && now - lastUpdateAt > POWER_OFF_AFTER_MS) {
        display.einkOff();
        panelOn = false;
    }

    if (partialsSinceClean >= CLEAN_MIN_PARTIALS && now - lastChangeAt > CLEAN_AFTER_IDLE_MS) {
        uint32_t t0 = millis();
        draw();
        display.display();
        Serial.printf("full refresh after %u partials: %lu ms\n", partialsSinceClean,
                      (unsigned long)(millis() - t0));
        partialsSinceClean = 0;
        panelOn = false;
    }

    // Script finished: rest, then start over.
    if (!SCRIPT[scriptPos] && now - lastChangeAt > 15000) {
        text.clear();
        scriptPos = 0;
        updates = 0;
        draw();
        display.display();
        partialsSinceClean = 0;
        nextKeyAt = millis() + 1000;
    }
}
