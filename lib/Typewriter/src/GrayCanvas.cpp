#include "GrayCanvas.h"

#include <esp_heap_caps.h>
#include <string.h>

namespace tw {

bool GrayCanvas::begin() {
    if (!pixels_)
        pixels_ = (uint8_t *)heap_caps_malloc((size_t)width_ * height_, MALLOC_CAP_SPIRAM);
    if (pixels_)
        memset(pixels_, 0, (size_t)width_ * height_);
    return pixels_ != nullptr;
}

void GrayCanvas::end() {
    free(pixels_);
    pixels_ = nullptr;
}

void GrayCanvas::show(Inkplate &display) {
    display.selectDisplayMode(INKPLATE_3BIT);
    display.clearDisplay();
    for (int32_t y = 0; y < height_; y++) {
        const uint8_t *row = pixels_ + y * width_;
        for (int32_t x = 0; x < width_; x++) {
            if (row[x]) // 3-bit colors run from 0 (black) to 7 (white)
                display.drawPixel(x, y, 7 - (row[x] * 7 + 127) / 255);
        }
    }
    display.display();
}

} // namespace tw
