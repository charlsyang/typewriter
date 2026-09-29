// Step 1: show each candidate font's writing screen on the real panel,
// 12 seconds each, looping. Images come from tools/preview_fonts.py.

#include "Inkplate.h"
#include "previews.h"

Inkplate display(INKPLATE_1BIT);

void setup() {
    display.begin();
}

void loop() {
    for (int i = 0; i < PREVIEW_COUNT; i++) {
        display.clearDisplay();
        display.drawBitmap(0, 0, PREVIEWS[i], 1280, 720, BLACK);
        display.display();
        delay(12000);
    }
}
