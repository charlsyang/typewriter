// An 8-bit ink canvas in PSRAM for drawing anti-aliased text, shown on the panel
// in its 3-bit (8 gray) mode with one full refresh. Used for the sleep screen.
//
// Glyphs are composited by keeping the darker ink, so kerned letters that
// overlap don't lighten each other's edges.
#pragma once

#include <Inkplate.h>
#include <stdint.h>

namespace tw {

class GrayCanvas {
  public:
    GrayCanvas(int16_t width, int16_t height) : width_(width), height_(height) {}
    ~GrayCanvas() { end(); }

    // Allocates the canvas (width × height bytes of PSRAM), cleared. False if out of memory.
    bool begin();
    void end();

    int16_t width() const { return width_; }
    int16_t height() const { return height_; }

    // Adds ink (0 none, 255 full) at a pixel, keeping whichever is darker.
    void ink(int32_t x, int32_t y, uint8_t amount) {
        if (x >= 0 && y >= 0 && x < width_ && y < height_) {
            uint8_t &p = pixels_[y * width_ + x];
            if (amount > p)
                p = amount;
        }
    }

    // Draws the canvas into the panel's 3-bit buffer and refreshes the panel. The
    // display is left in 3-bit mode; select INKPLATE_1BIT again to go back.
    void show(Inkplate &display);

  private:
    int16_t width_, height_;
    uint8_t *pixels_ = nullptr;
};

} // namespace tw
