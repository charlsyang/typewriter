#!/usr/bin/env python3
"""How your highlights would fit the sleep screen at different text sizes.

Wraps every highlight in build/highlights/highlights.jsonl exactly as the device
does (advances and kerning read from the generated lib/Typewriter/src/fonts/
Libron*.h headers, ligatures included), then, for each ladder of sizes, reports
how many highlights land at each size and how many must be cut with "…".

Layout and cleaning follow the firmware (drawHighlight() in typewriter.ino and
lib/Typewriter/src/Highlight.cpp): the same padding on all four sides, from the
quote's cap height at the top to the author's descenders at the bottom; the
title 30 px below the line after the last, the author 40 px below the title.

Usage: tools/highlight_sizes.py [--samples]
  --samples  also list, per size, highlight indexes just inside the limit, for
             checking on the panel with Ctrl+R "hltest <size> <index>"
"""
import argparse
import json
import os
import re

ROOT = os.path.join(os.path.dirname(__file__), "..")
FONTS = os.path.join(ROOT, "lib", "Typewriter", "src", "fonts")
LADDERS = [(52, 44, 36), (44, 36)]  # 36 px is the floor (user); add sizes after generating their Libron headers
W, H, PAD, TITLE_GAP, AUTHOR_GAP = 1280, 720, 80, 30, 40
EXTRA = [0x2018, 0x2019, 0x201A, 0x201C, 0x201D, 0x201E, 0x2013, 0x2014, 0x2026, 0x2022, 0x2032, 0x2033,
         0x2039, 0x203A, 0x20AC, 0x2122, 0x2190, 0x2192, 0x2191, 0x2193, 0x2212, 0x2248, 0x2260, 0x2264,
         0x2265, 0xFB00, 0xFB01, 0xFB02, 0xFB03, 0xFB04]
LIGATURES = [("ffi", 0xFB03), ("ffl", 0xFB04), ("ff", 0xFB00), ("fi", 0xFB01), ("fl", 0xFB02)]


def font_byte(cp):
    if 0x20 <= cp <= 0x7E or 0xA0 <= cp <= 0xFF:
        return cp
    return 0x80 + EXTRA.index(cp) if cp in EXTRA else ord("?")


class Font:
    def __init__(self, size):
        src = open(os.path.join(FONTS, f"Libron{size}.h")).read()
        glyphs = [tuple(map(int, m.groups())) for m in
                  re.finditer(r"^  \{\d+, (\d+), (\d+), (\d+), (-?\d+), (-?\d+)\},", src, re.M)]
        self.adv = [g[2] for g in glyphs]
        self.cap = -glyphs[ord("H") - 0x20][4]
        self.desc = glyphs[ord("p") - 0x20][1] + glyphs[ord("p") - 0x20][4]
        self.kern = {(int(a, 16), int(b, 16)): int(d) for a, b, d in re.findall(r"\{0x(..),0x(..),(-?\d+)\}", src)}
        self.line = int(re.search(r"0x20, 0xFF, (\d+),", src).group(1))
        self.size = size

    def glyphs(self, s):
        i = 0
        while i < len(s):
            for seq, cp in LIGATURES:
                if s.startswith(seq, i) and self.adv[font_byte(cp) - 0x20]:
                    yield font_byte(cp)
                    i += len(seq)
                    break
            else:
                yield font_byte(ord(s[i]))
                i += 1

    def width16(self, s):
        w, prev = 0, None
        for b in self.glyphs(s):
            w += self.kern.get((prev, b), 0) + self.adv[b - 0x20]
            prev = b
        return w

    def wrap(self, text):
        """As tw::highlight::wrap(): greedy, " >" kept on the word before it. (The
        device then evens the lines out with balance(), which keeps their number.)"""
        lines = []
        for para in text.split("\n"):
            pieces = re.findall(r"\S+(?: >(?= |$))?", para)
            cur = ""
            for piece in pieces:
                t = piece if not cur else cur + " " + piece
                if cur and self.width16(t) > (W - 2 * PAD) * 16:
                    lines.append(cur)
                    cur = piece
                else:
                    cur = t
            lines.append(cur)
        return lines

    def max_lines(self, attribution_desc):
        n = 0
        while PAD + self.cap + (n + 1) * self.line + TITLE_GAP + AUTHOR_GAP <= H - PAD - attribution_desc:
            n += 1
        return n

    def has_glyph(self, ch):
        b = font_byte(ord(ch))
        return (b != ord("?") or ch == "?") and self.adv[b - 0x20] > 0


def clean(text, has_glyph):
    """As tw::highlight::clean()."""
    text = re.sub(r"!?\[([^\]]*)\]\([^)]*\)", r"\1", text)
    text = text.replace("**", "").replace("__", "").replace("*", "").replace("\r", "")
    out, space, brk = "", False, False
    for ch in text:
        cp = ord(ch)
        if ch == "\n":
            brk = True
        elif ch in " \t" or 0x2000 <= cp <= 0x200A or cp in (0x202F, 0x205F, 0x3000):
            space = True
        elif has_glyph(ch):
            if out:
                out += " > " if brk else " " if space else ""
            out += ch
            space = brk = False
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--samples", action="store_true")
    args = ap.parse_args()
    recs = [json.loads(l) for l in open(os.path.join(ROOT, "build", "highlights", "highlights.jsonl"), encoding="utf-8")]
    sizes = sorted({s for l in LADDERS for s in l}, reverse=True)
    fonts = {s: Font(s) for s in sizes}
    attribution = Font(28)  # title and author
    texts = [clean(r["text"], attribution.has_glyph) for r in recs]
    room = {s: fonts[s].max_lines(attribution.desc) for s in sizes}
    lines = {s: [len(fonts[s].wrap(t)) for t in texts] for s in sizes}
    print("lines that fit: " + ", ".join(f"{s}px {room[s]}" for s in sizes))
    for ladder in LADDERS:
        counts, cut = {s: 0 for s in ladder}, 0
        for i in range(len(texts)):
            for s in ladder:
                if lines[s][i] <= room[s]:
                    counts[s] += 1
                    break
            else:
                cut += 1
        n = len(texts)
        parts = "  ".join(f"{s}px {100 * counts[s] / n:4.1f}%" for s in ladder)
        print(f"{'/'.join(map(str, ladder)):>12}:  {parts}  don't fit {100 * cut / n:4.1f}% ({cut})")
    if args.samples:
        for s in sizes:
            m = room[s]
            near = [i for i in range(len(texts)) if lines[s][i] == m][:3]
            over = [i for i in range(len(texts)) if lines[s][i] == m + 1][:2]
            print(f"{s}px: exactly {m} lines {near}; one line too many {over}")


if __name__ == "__main__":
    main()
