#include "Text.h"

#include "GrayCanvas.h"
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

int16_t kerning(const GrayFont &font, uint8_t left, uint8_t right) {
    uint16_t key = left << 8 | right;
    size_t lo = 0, hi = font.kernCount;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        uint16_t k = font.kern[mid].left << 8 | font.kern[mid].right;
        if (k == key)
            return font.kern[mid].dx16;
        if (k < key)
            lo = mid + 1;
        else
            hi = mid;
    }
    return 0;
}

// The next glyph of s for a gray font: like fontByte(nextCodepoint()), but ff, fi,
// fl, ffi and ffl become the font's ligatures, so their strokes don't collide.
static uint8_t nextGrayByte(const GrayFont &font, const char *s, size_t end, size_t &i) {
    if (s[i] == 'f' && i + 1 < end) {
        char b = s[i + 1], c = i + 2 < end ? s[i + 2] : 0;
        uint32_t lig = 0;
        size_t len = 0;
        if (b == 'f' && (c == 'i' || c == 'l'))
            lig = c == 'i' ? 0xFB03 : 0xFB04, len = 3;
        else if (b == 'f' || b == 'i' || b == 'l')
            lig = b == 'f' ? 0xFB00 : b == 'i' ? 0xFB01 : 0xFB02, len = 2;
        uint8_t byte = lig ? fontByte(lig) : '?';
        if (lig && byte != '?' && font.glyph[byte - font.first].xAdvance16) {
            i += len;
            return byte;
        }
    }
    return fontByte(nextCodepoint(s, end, i));
}

int32_t measure16(const GrayFont &font, const char *s, size_t start, size_t end) {
    int32_t w = 0;
    int prev = -1;
    size_t i = start;
    while (i < end) {
        uint8_t b = nextGrayByte(font, s, end, i);
        if (prev >= 0)
            w += kerning(font, prev, b);
        w += font.glyph[b - font.first].xAdvance16;
        prev = b;
    }
    return w;
}

int32_t drawText(GrayCanvas &canvas, const GrayFont &font, int32_t x, int32_t y, const char *s, size_t start,
                 size_t end) {
    int32_t x16 = x * 16; // pen position in 1/16 px; each glyph lands on the nearest whole pixel
    int prev = -1;
    size_t i = start;
    while (i < end) {
        uint8_t b = nextGrayByte(font, s, end, i);
        if (prev >= 0)
            x16 += kerning(font, prev, b);
        const GrayGlyph &g = font.glyph[b - font.first];
        int32_t gx = (x16 + 8) / 16 + g.xOffset, gy = y + g.yOffset;
        const uint8_t *bits = font.bitmap + g.bitmapOffset;
        for (int r = 0; r < g.height; r++) {
            for (int c = 0; c < g.width; c++) {
                size_t n = r * g.width + c;
                uint8_t ink = n % 2 ? bits[n / 2] & 0x0F : bits[n / 2] >> 4;
                if (ink)
                    canvas.ink(gx + c, gy + r, ink * 17);
            }
        }
        x16 += g.xAdvance16;
        prev = b;
    }
    return (x16 + 8) / 16;
}

int32_t drawText(GrayCanvas &canvas, const GFXfont &font, int32_t x, int32_t y, const char *s, size_t start,
                 size_t end) {
    size_t i = start;
    while (i < end) {
        uint8_t b = fontByte(nextCodepoint(s, end, i));
        const GFXglyph &g = font.glyph[b - font.first];
        const uint8_t *bits = font.bitmap + g.bitmapOffset;
        for (int n = 0; n < g.width * g.height; n++) {
            if (bits[n / 8] & (0x80 >> (n % 8)))
                canvas.ink(x + g.xOffset + n % g.width, y + g.yOffset + n / g.width, 255);
        }
        x += g.xAdvance;
    }
    return x;
}

} // namespace tw
