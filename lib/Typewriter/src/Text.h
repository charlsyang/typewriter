// UTF-8 text measured and drawn with the generated GFX fonts (see tools/make_font.py).
#pragma once

#include <Inkplate.h>
#include <stddef.h>
#include <stdint.h>

namespace tw {

// Decodes the code point starting at s[i] and advances i past it.
// Malformed or truncated sequences decode as U+FFFD, one byte at a time.
uint32_t nextCodepoint(const char *s, size_t len, size_t &i);

// The font byte that draws a code point, or '?' when the font has no glyph for it.
uint8_t fontByte(uint32_t cp);

// Horizontal advance of one code point in pixels.
int16_t advance(const GFXfont &font, uint32_t cp);

// Width in pixels of s[start, end).
int32_t measure(const GFXfont &font, const char *s, size_t start, size_t end);

// Draws s[start, end) with its baseline at y; returns the x after the last glyph.
int32_t drawText(Inkplate &display, const GFXfont &font, int32_t x, int32_t y, const char *s, size_t start,
                 size_t end);

// Anti-aliased fonts for 3-bit mode (tools/make_gray_font.py), with kerning.
struct GrayGlyph {
    uint32_t bitmapOffset; // into the font's bitmap: 4 bits of ink per pixel, high nibble first
    uint8_t width, height;
    uint16_t xAdvance16; // in 1/16 px
    int8_t xOffset, yOffset;
};

struct KernPair {
    uint8_t left, right; // font bytes (see fontByte)
    int16_t dx16;        // added between them, in 1/16 px
};

struct GrayFont {
    const uint8_t *bitmap;
    const GrayGlyph *glyph;
    uint8_t first, last;
    uint8_t yAdvance;
    int16_t ascent, descent;
    const KernPair *kern; // sorted by (left, right)
    uint16_t kernCount;
};

class GrayCanvas;

// Kerning between two font bytes, in 1/16 px.
int16_t kerning(const GrayFont &font, uint8_t left, uint8_t right);

// Width of s[start, end) in 1/16 px, kerning included.
int32_t measure16(const GrayFont &font, const char *s, size_t start, size_t end);

// Draws s[start, end) with its baseline at y; returns the x after the last glyph.
int32_t drawText(GrayCanvas &canvas, const GrayFont &font, int32_t x, int32_t y, const char *s, size_t start,
                 size_t end);

// A 1-bit GFX font drawn into the canvas at full ink.
int32_t drawText(GrayCanvas &canvas, const GFXfont &font, int32_t x, int32_t y, const char *s, size_t start,
                 size_t end);

} // namespace tw
