#include "Page.h"

#include "Text.h"

namespace tw {

Page::Page(Inkplate &display, const GFXfont &body, int16_t bodyAscent, const GFXfont &small, int16_t smallAscent,
           Layout layout)
    : display_(display), body_(body), small_(small), bodyAscent_(bodyAscent), smallAscent_(smallAscent),
      layout_(layout) {
}

int16_t Page::lineHeight(const Line &line) const {
    return line.start == line.end ? body_.yAdvance / 2 : body_.yAdvance;
}

// Greedy word wrap of one paragraph (no '\n' inside). A word wider than the
// line is broken at the last character that fits.
void Page::wrapParagraph(const std::string &text, size_t start, size_t end, std::vector<Line> &out) const {
    const int32_t width = display_.width() - 2 * layout_.marginX;
    if (start == end) {
        out.push_back({start, end});
        return;
    }
    const char *s = text.c_str();
    size_t lineStart = start;
    size_t lastBreak = 0; // byte index just after the last space on this line
    int32_t x = 0;
    size_t i = start;
    while (i < end) {
        size_t charStart = i;
        uint32_t cp = nextCodepoint(s, end, i);
        int16_t w = advance(body_, cp);
        if (cp == ' ') {
            x += w;
            lastBreak = i;
            continue;
        }
        if (x + w > width && charStart > lineStart) {
            size_t breakAt = lastBreak > lineStart ? lastBreak : charStart;
            out.push_back({lineStart, breakAt});
            lineStart = breakAt;
            lastBreak = 0;
            x = measure(body_, s, lineStart, i);
            continue;
        }
        x += w;
    }
    out.push_back({lineStart, end});
}

std::vector<Line> Page::visibleLines(const std::string &text) const {
    const int32_t room = layout_.textBottom - layout_.marginTop;
    std::vector<Line> lines;
    int32_t used = 0;
    size_t paraEnd = text.size();
    // Wrap whole paragraphs from the end until the screen is full.
    while (true) {
        size_t nl = paraEnd == 0 ? std::string::npos : text.rfind('\n', paraEnd - 1);
        size_t paraStart = nl == std::string::npos ? 0 : nl + 1;
        std::vector<Line> para;
        wrapParagraph(text, paraStart, paraEnd, para);
        for (auto it = para.rbegin(); it != para.rend(); ++it) {
            int16_t h = lineHeight(*it);
            if (used + h > room)
                return lines;
            lines.insert(lines.begin(), *it);
            used += h;
        }
        if (paraStart == 0)
            return lines;
        paraEnd = paraStart - 1;
    }
}

std::vector<Line> Page::linesFrom(const std::string &text, size_t top) const {
    std::vector<Line> lines;
    size_t paraStart = top;
    while (true) {
        size_t nl = text.find('\n', paraStart);
        size_t paraEnd = nl == std::string::npos ? text.size() : nl;
        wrapParagraph(text, paraStart, paraEnd, lines);
        if (nl == std::string::npos)
            return lines;
        paraStart = nl + 1;
    }
}

int32_t Page::height(const std::vector<Line> &lines) const {
    int32_t h = 0;
    for (const Line &line : lines)
        h += lineHeight(line);
    // The last line holds the cursor, so it always needs full height.
    if (!lines.empty() && lines.back().start == lines.back().end)
        h += body_.yAdvance - lineHeight(lines.back());
    return h;
}

void Page::showEnd(const std::string &text) {
    std::vector<Line> tail = visibleLines(text);
    top_ = tail.empty() ? 0 : tail.front().start;
}

bool Page::render(const std::string &text, const char *footer) {
    const int32_t room = layout_.textBottom - layout_.marginTop;
    bool turned = false;

    // Deleted back past the top of the page: show the end of the text instead.
    if (top_ > text.size()) {
        std::vector<Line> tail = visibleLines(text);
        top_ = tail.empty() ? 0 : tail.front().start;
        turned = true;
    }

    // Lines stay put until the page is full; then turn to a fresh page that
    // repeats the last few lines for context.
    std::vector<Line> lines = linesFrom(text, top_);
    if (height(lines) > room) {
        size_t keepFrom = lines.size() > layout_.carryLines + 1 ? lines.size() - 1 - layout_.carryLines : 0;
        top_ = lines[keepFrom].start;
        lines.erase(lines.begin(), lines.begin() + keepFrom);
        if (height(lines) > room) { // a huge paste: fall back to the newest lines
            lines = visibleLines(text);
            top_ = lines.empty() ? 0 : lines.front().start;
        }
        turned = true;
    }

    display_.clearDisplay();
    const char *s = text.c_str();

    // Skip blank lines at the top of the screen.
    size_t first = 0;
    while (first + 1 < lines.size() && lines[first].start == lines[first].end)
        first++;

    int32_t y = layout_.marginTop;
    int32_t cursorX = layout_.marginX;
    int32_t cursorY = y;
    for (size_t k = first; k < lines.size(); k++) {
        cursorX = drawText(display_, body_, layout_.marginX, y + bodyAscent_, s, lines[k].start, lines[k].end);
        cursorY = y;
        y += lineHeight(lines[k]);
    }

    // Cursor: a slim bar just after the last character.
    const int16_t size = body_.yAdvance * 2 / 3; // font size in px (line spacing is 1.5)
    display_.fillRect(cursorX + 4, cursorY + size * 15 / 100, 3, size * 90 / 100, BLACK);

    if (footer && *footer)
        drawText(display_, small_, layout_.marginX, layout_.footerTop + smallAscent_, footer, 0, strlen(footer));
    return turned;
}

} // namespace tw
