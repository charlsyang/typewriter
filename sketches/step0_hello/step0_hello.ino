// Step 0: confirm the toolchain works, pick the landscape rotation,
// and report the board's memory on screen and over serial.

#ifndef ARDUINO_INKPLATE5V2
#error "Select the Soldered Inkplate5V2 board."
#endif

#include "Inkplate.h"
#include <esp_chip_info.h>

Inkplate display(INKPLATE_1BIT);

void setup() {
    Serial.begin(115200);
    display.begin();

    // Use the first rotation that makes the screen wider than tall.
    uint8_t rotation = 0;
    for (uint8_t r = 0; r < 4; r++) {
        display.setRotation(r);
        if (display.width() > display.height()) {
            rotation = r;
            break;
        }
    }
    display.setRotation(rotation);

    char info[5][64];
    snprintf(info[0], sizeof(info[0]), "Screen: %d x %d (rotation %d)", display.width(), display.height(), rotation);
    snprintf(info[1], sizeof(info[1]), "Flash: %lu MB", (unsigned long)(ESP.getFlashChipSize() / (1024 * 1024)));
    snprintf(info[2], sizeof(info[2]), "PSRAM: %lu KB total, %lu KB free", (unsigned long)(ESP.getPsramSize() / 1024),
             (unsigned long)(ESP.getFreePsram() / 1024));
    snprintf(info[3], sizeof(info[3]), "Heap: %lu KB free", (unsigned long)(ESP.getFreeHeap() / 1024));
    snprintf(info[4], sizeof(info[4]), "Chip: %s rev %d, %d MHz", ESP.getChipModel(), ESP.getChipRevision(),
             ESP.getCpuFreqMHz());

    display.clearDisplay();
    display.setTextColor(BLACK);
    display.setTextSize(6);
    display.setCursor(40, 40);
    display.print("Hello, typewriter.");

    display.setTextSize(3);
    for (int i = 0; i < 5; i++) {
        display.setCursor(40, 200 + i * 50);
        display.print(info[i]);
        Serial.println(info[i]);
    }

    display.setTextSize(2);
    display.setCursor(40, display.height() - 40);
    display.print("This edge should be at the bottom.");
    display.display();
}

void loop() {
}
