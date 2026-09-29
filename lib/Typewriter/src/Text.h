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

} // namespace tw
