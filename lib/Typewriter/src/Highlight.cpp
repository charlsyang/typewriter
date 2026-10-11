#include "Highlight.h"

#include <cstdint>

namespace tw {
namespace highlight {

namespace {

// Decodes one UTF-8 code point at s[i], advancing i. Bad bytes come back as U+FFFD.
uint32_t decode(const std::string &s, size_t &i) {
    uint8_t b = s[i];
    int extra = b < 0x80 ? 0 : (b & 0xE0) == 0xC0 ? 1 : (b & 0xF0) == 0xE0 ? 2 : (b & 0xF8) == 0xF0 ? 3 : -1;
    if (extra < 0 || i + extra >= s.size() + (extra == 0 ? 1 : 0)) {
        i++;
        return 0xFFFD;
    }
    uint32_t cp = extra == 0 ? b : b & (0x3F >> extra);
    for (int k = 1; k <= extra; k++) {
        uint8_t c = s[i + k];
        if ((c & 0xC0) != 0x80) {
            i++;
            return 0xFFFD;
        }
        cp = cp << 6 | (c & 0x3F);
    }
    i += extra + 1;
    return cp;
}

bool isUnicodeSpace(uint32_t cp) {
    return cp == '\t' || (cp >= 0x2000 && cp <= 0x200A) || cp == 0x202F || cp == 0x205F || cp == 0x3000;
}

// [words](url) -> words, ![alt](url) -> alt; anything else left as is.
std::string unlink(const std::string &s) {
    std::string out;
    size_t i = 0;
    while (i < s.size()) {
        size_t open = s[i] == '!' && i + 1 < s.size() && s[i + 1] == '[' ? i + 1 : s[i] == '[' ? i : std::string::npos;
        if (open != std::string::npos) {
            size_t close = s.find(']', open);
            if (close != std::string::npos && close + 1 < s.size() && s[close + 1] == '(') {
                size_t end = s.find(')', close);
                if (end != std::string::npos) {
                    out += s.substr(open + 1, close - open - 1);
                    i = end + 1;
                    continue;
                }
            }
        }
        out += s[i++];
    }
    return out;
}

} // namespace

std::string clean(const std::string &raw, const std::function<bool(uint32_t)> &hasGlyph) {
    std::string s = unlink(raw);
    std::string out;
    bool pendingSpace = false, pendingBreak = false;
    size_t i = 0;
    while (i < s.size()) {
        if (s.compare(i, 2, "**") == 0 || s.compare(i, 2, "__") == 0) {
            i += 2;
            continue;
        }
        uint32_t cp = decode(s, i);
        if (cp == '*' || cp == '\r')
            continue;
        if (cp == '\n') {
            pendingBreak = true;
            continue;
        }
        if (cp == ' ' || isUnicodeSpace(cp)) {
            pendingSpace = true;
            continue;
        }
        if (!hasGlyph(cp))
            continue; // emoji, joiners, anything the font can't draw
        if (!out.empty()) {
            if (pendingBreak)
                out += " > ";
            else if (pendingSpace)
                out += ' ';
        }
        pendingSpace = pendingBreak = false;
        size_t len = cp < 0x80 ? 1 : cp < 0x800 ? 2 : cp < 0x10000 ? 3 : 4;
        out.append(s, i - len, len);
    }
    return out;
}

namespace {

// The pieces a line can break between: words, with a " >" (a collapsed line
// break, see clean()) kept on the word before it so no line starts with ">".
// Returns [start, end) of each piece within para.
std::vector<std::pair<size_t, size_t>> pieces(const std::string &para) {
    std::vector<std::pair<size_t, size_t>> out;
    size_t start = 0;
    for (size_t i = 0; i <= para.size(); i++) {
        if (i < para.size() && para[i] != ' ')
            continue;
        bool beforeArrow = i < para.size() && para.compare(i, 2, " >") == 0 && (i + 2 == para.size() || para[i + 2] == ' ');
        if (beforeArrow)
            continue; // not a break: the ">" stays with this word
        if (i > start)
            out.push_back({start, i});
        start = i + 1;
    }
    return out;
}

// Greedy: as many pieces on each line as fit. Returns the index of the first
// piece of each line.
std::vector<size_t> greedyBreaks(const std::string &para, const std::vector<std::pair<size_t, size_t>> &p,
                                 const Size &size, int32_t max16) {
    std::vector<size_t> starts;
    size_t a = 0;
    while (a < p.size()) {
        starts.push_back(a);
        size_t b = a;
        while (b + 1 < p.size() && size.width16(para, p[a].first, p[b + 1].second) <= max16)
            b++;
        a = b + 1;
    }
    return starts;
}

// Balanced: the same number of lines as greedy, with the breaks that make the
// lines as even as possible (least sum of squared leftover width, last line
// included), and at least two pieces on the last line. Falls back to greedy if
// there's no such layout.
std::vector<size_t> balancedBreaks(const std::string &para, const std::vector<std::pair<size_t, size_t>> &p,
                                   const Size &size, int32_t max16) {
    std::vector<size_t> greedy = greedyBreaks(para, p, size, max16);
    const size_t n = p.size(), lines = greedy.size();
    if (lines < 2)
        return greedy;
    const int64_t INF = INT64_MAX / 4;
    // cost[k][b]: pieces 0..b on k + 1 lines, the last of them ending at b.
    std::vector<std::vector<int64_t>> cost(lines, std::vector<int64_t>(n, INF));
    std::vector<std::vector<size_t>> from(lines, std::vector<size_t>(n, 0));
    for (size_t b = 0; b < n; b++) {
        for (size_t a = b + 1; a-- > 0;) { // line = pieces a..b, longest last
            int32_t w = size.width16(para, p[a].first, p[b].second);
            if (w > max16 && a < b)
                break; // wider still for smaller a
            int64_t slack = w > max16 ? 0 : (max16 - w) / 16;
            int64_t line = slack * slack;
            if (b == n - 1 && b - a < 1)
                continue; // the last line needs two pieces
            if (a == 0) {
                if (line < cost[0][b])
                    cost[0][b] = line, from[0][b] = 0;
                continue;
            }
            for (size_t k = 1; k < lines; k++) {
                if (cost[k - 1][a - 1] >= INF)
                    continue;
                int64_t c = cost[k - 1][a - 1] + line;
                if (c < cost[k][b])
                    cost[k][b] = c, from[k][b] = a;
            }
        }
    }
    if (cost[lines - 1][n - 1] >= INF)
        return greedy;
    std::vector<size_t> starts(lines);
    size_t b = n - 1;
    for (size_t k = lines; k-- > 0;) {
        starts[k] = from[k][b];
        if (k)
            b = starts[k] - 1;
    }
    return starts;
}

std::vector<std::string> layoutLines(const std::string &text, const Size &size, int32_t maxWidth, bool balance) {
    std::vector<std::string> lines;
    const int32_t max16 = maxWidth * 16;
    size_t paraStart = 0;
    while (paraStart <= text.size()) {
        size_t paraEnd = text.find('\n', paraStart);
        if (paraEnd == std::string::npos)
            paraEnd = text.size();
        std::string para = text.substr(paraStart, paraEnd - paraStart);
        auto p = pieces(para);
        if (p.empty()) {
            lines.push_back("");
        } else {
            auto starts = balance ? balancedBreaks(para, p, size, max16) : greedyBreaks(para, p, size, max16);
            for (size_t k = 0; k < starts.size(); k++) {
                size_t last = k + 1 < starts.size() ? starts[k + 1] - 1 : p.size() - 1;
                lines.push_back(para.substr(p[starts[k]].first, p[last].second - p[starts[k]].first));
            }
        }
        paraStart = paraEnd + 1;
    }
    return lines;
}

} // namespace

std::vector<std::string> wrap(const std::string &text, const Size &size, int32_t maxWidth) {
    return layoutLines(text, size, maxWidth, false);
}

std::vector<std::string> balance(const std::string &text, const Size &size, int32_t maxWidth) {
    return layoutLines(text, size, maxWidth, true);
}

bool truncate(std::vector<std::string> &lines, size_t maxLines, const Size &size, int32_t maxWidth) {
    if (lines.size() <= maxLines)
        return false;
    lines.resize(maxLines);
    if (lines.empty())
        return true;
    std::string last = lines.back();
    const std::string ellipsis = "\xE2\x80\xA6"; // …
    auto fits = [&](const std::string &s) { return size.width16(s, 0, s.size()) <= maxWidth * 16; };
    auto trimEnd = [](std::string &s) {
        while (!s.empty()) {
            unsigned char c = s.back();
            if (c == ' ' || c == ',' || c == ';' || c == ':' || c == '-') {
                s.pop_back();
            } else if (s.size() >= 3 && s.compare(s.size() - 3, 2, "\xE2\x80") == 0 &&
                       (c == 0x93 || c == 0x94)) { // – —
                s.resize(s.size() - 3);
            } else {
                break;
            }
        }
    };
    trimEnd(last);
    while (!fits(last + ellipsis)) {
        size_t space = last.rfind(' ');
        if (space == std::string::npos)
            break;
        last.resize(space);
        trimEnd(last);
    }
    lines.back() = last + ellipsis;
    return true;
}

Layout fit(const std::string &text, const std::vector<Size> &sizes, const std::function<size_t(size_t)> &maxLines,
           int32_t maxWidth) {
    Layout layout;
    for (size_t k = 0; k < sizes.size(); k++) {
        layout.sizeIndex = k;
        layout.lines = wrap(text, sizes[k], maxWidth);
        layout.fits = layout.lines.size() <= maxLines(k);
        if (layout.fits) {
            layout.lines = balance(text, sizes[k], maxWidth); // same number of lines, evened out
            break;
        }
    }
    return layout;
}

} // namespace highlight
} // namespace tw
