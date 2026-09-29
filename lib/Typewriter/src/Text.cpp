#include "Text.h"

#include "fonts/FontExtraCodepoints.h"

namespace tw {

uint32_t nextCodepoint(const char *s, size_t len, size_t &i) {
    uint8_t b = s[i];
    int extra;
    uint32_t cp;
    if (b < 0x80) {
        i++;
        return b;
    } else if ((b & 0xE0) == 0xC0) {
        extra = 1;
        cp = b & 0x1F;
    } else if ((b & 0xF0) == 0xE0) {
        extra = 2;
        cp = b & 0x0F;
    } else if ((b & 0xF8) == 0xF0) {
        extra = 3;
        cp = b & 0x07;
    } else {
        i++;
        return 0xFFFD;
    }
    if (i + extra >= len) {
        i++;
        return 0xFFFD;
    }
    for (int k = 1; k <= extra; k++) {
        uint8_t c = s[i + k];
        if ((c & 0xC0) != 0x80) {
            i++;
            return 0xFFFD;
        }
        cp = (cp << 6) | (c & 0x3F);
    }
    i += extra + 1;
    return cp;
}

uint8_t fontByte(uint32_t cp) {
    if (cp >= 0x20 && cp <= 0x7E)
        return cp;
    if (cp >= 0xA0 && cp <= 0xFF)
        return cp;
    for (int k = 0; k < FONT_EXTRA_COUNT; k++) {
        if (FONT_EXTRA_CODEPOINTS[k] == cp)
            return 0x80 + k;
    }
    return '?';
}

int16_t advance(const GFXfont &font, uint32_t cp) {
    return font.glyph[fontByte(cp) - font.first].xAdvance;
}

int32_t measure(const GFXfont &font, const char *s, size_t start, size_t end) {
    int32_t w = 0;
    size_t i = start;
    while (i < end)
        w += advance(font, nextCodepoint(s, end, i));
    return w;
}

int32_t drawText(Inkplate &display, const GFXfont &font, int32_t x, int32_t y, const char *s, size_t start,
                 size_t end) {
    display.setFont(&font);
    size_t i = start;
    while (i < end) {
        uint8_t b = fontByte(nextCodepoint(s, end, i));
        display.drawChar(x, y, b, BLACK, WHITE, 1);
        x += font.glyph[b - font.first].xAdvance;
    }
    return x;
}

} // namespace tw
