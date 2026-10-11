// Laying out a Readwise highlight for the sleep screen: cleaning its text, then
// picking the largest text size that fits and wrapping it. A highlight too long
// even at the smallest size isn't shown. Plain C++ so it's tested on the Mac
// (test/highlight_test.cpp); tools/highlight_sizes.py follows the same rules.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace tw {
namespace highlight {

// Readwise text as it should be shown, as one paragraph: [words](url) becomes
// words, ** and __ and * go, runs of spaces become one, line breaks (however
// many in a row) become " > ", and characters for which hasGlyph() is false are
// dropped (emoji, zero-width joiners) or, for other spaces, become a plain space.
std::string clean(const std::string &raw, const std::function<bool(uint32_t)> &hasGlyph);

// One text size: its line height and the width of s[start, end) in 1/16 px.
struct Size {
    int px;
    int lineHeight;
    std::function<int32_t(const std::string &, size_t, size_t)> width16;
};

// Greedy word wrap into lines no wider than maxWidth (px); a line break in the
// text always starts a new line. A single word wider than a line gets its own.
// A " >" (see clean()) stays on the word before it: no line starts with ">".
std::vector<std::string> wrap(const std::string &text, const Size &size, int32_t maxWidth);

// Like wrap(), with as many lines, but with the breaks that make the lines as
// even as possible (least sum of squared leftover width, the last line too) and
// never a single word alone on the last line.
std::vector<std::string> balance(const std::string &text, const Size &size, int32_t maxWidth);

// Cuts lines to at most maxLines, ending the last with "…" (fitting maxWidth,
// trailing , ; : and dashes dropped before it). Returns true if it cut.
bool truncate(std::vector<std::string> &lines, size_t maxLines, const Size &size, int32_t maxWidth);

struct Layout {
    bool fits = false;
    size_t sizeIndex = 0; // into the sizes given to fit()
    std::vector<std::string> lines;
};

// Tries sizes in order (largest first); maxLines(i) is how many lines size i has
// room for, counted with wrap(). The lines that fit are then balance()d. fits is
// false if the text doesn't fit even at the last size.
Layout fit(const std::string &text, const std::vector<Size> &sizes, const std::function<size_t(size_t)> &maxLines,
           int32_t maxWidth);

} // namespace highlight
} // namespace tw
