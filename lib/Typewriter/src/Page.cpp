#include "Page.h"

#include "Text.h"

namespace tw {

Page::Page(Inkplate &display, const GFXfont &body, int16_t bodyAscent, const GFXfont &small, int16_t smallAscent,
           Layout layout)
    : display_(display), body_(body), small_(small), bodyAscent_(bodyAscent), smallAscent_(smallAscent),
      layout_(layout) {
}

// Empty lines too take a full line, so every full page ends at the same height.
int16_t Page::lineHeight(const Line &) const {
    return body_.yAdvance;
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
    return h;
}

size_t Page::pageEnd(const std::vector<Line> &lines, size_t start) const {
    const int32_t room = layout_.textBottom - layout_.marginTop;
    int32_t used = 0;
    size_t k = start;
    while (k < lines.size() && used + lineHeight(lines[k]) <= room)
        used += lineHeight(lines[k++]);
    return k > start ? k : start + 1;
}

Page::Reading Page::readingPages(const std::string &text) const {
    Reading r;
    r.lines = linesFrom(text, 0);
    // The writing page starts at the line beginning at top_.
    size_t writingStart = r.lines.size();
    for (size_t k = 0; k < r.lines.size(); k++) {
        if (r.lines[k].start >= top_) {
            writingStart = k;
            break;
        }
    }
    size_t start = 0;
    while (start < writingStart) {
        r.starts.push_back(start);
        size_t end = pageEnd(r.lines, start);
        size_t next = end > layout_.carryLines ? end - layout_.carryLines : end;
        start = next > start ? next : end;
    }
    return r;
}

void Page::renderReading(const std::string &text, const Reading &reading, size_t page, const char *footerLeft,
                         const char *footerRight) {
    shownValid_ = false; // the writing page has to be redrawn in full afterwards
    display_.clearDisplay();
    size_t first = reading.starts[page];
    // Skip blank lines at the top of the screen.
    while (first + 1 < reading.lines.size() && reading.lines[first].start == reading.lines[first].end)
        first++;
    size_t end = pageEnd(reading.lines, first);
    int32_t y = layout_.marginTop;
    for (size_t k = first; k < end; k++) {
        drawText(display_, body_, layout_.marginX, y + bodyAscent_, text.c_str(), reading.lines[k].start,
                 reading.lines[k].end);
        y += lineHeight(reading.lines[k]);
    }
    const int32_t baseline = layout_.footerTop + smallAscent_;
    if (footerLeft && *footerLeft)
        drawText(display_, small_, layout_.marginX, baseline, footerLeft, 0, strlen(footerLeft));
    if (footerRight && *footerRight) {
        size_t len = strlen(footerRight);
        int32_t w = measure(small_, footerRight, 0, len);
        drawText(display_, small_, display_.width() - layout_.marginX - w, baseline, footerRight, 0, len);
    }
}

void Page::showEnd(const std::string &text) {
    std::vector<Line> tail = visibleLines(text);
    top_ = tail.empty() ? 0 : tail.front().start;
    shownValid_ = false;
}

// Clears rows [y0, y1) to white. In 1-bit mode with no rotation each screen row
// is one row of the frame buffer, so this is a single memset; fillRect would set
// the pixels one at a time (hundreds of milliseconds for a large band).
void Page::clearRows(int32_t y0, int32_t y1) {
    y0 = max<int32_t>(y0, 0);
    y1 = min<int32_t>(y1, display_.height());
    if (y1 <= y0)
        return;
    if (display_.getRotation() == 0 && display_.getDisplayMode() == INKPLATE_1BIT) {
        const size_t rowBytes = E_INK_WIDTH / 8;
        memset(display_._partial + rowBytes * y0, 0, rowBytes * (y1 - y0));
    } else {
        display_.fillRect(0, y0, display_.width(), y1 - y0, WHITE);
    }
}

bool Page::render(const std::string &text, const char *footerLeft, const char *footerRight) {
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

    const char *s = text.c_str();

    // Skip blank lines at the top of the screen.
    size_t first = 0;
    while (first + 1 < lines.size() && lines[first].start == lines[first].end)
        first++;

    // Redraw from the first line that differs from what's on screen (always
    // including the last line, where the cursor is), or everything if the page
    // moved or something else was drawn. The line that was last on screen had the
    // cursor drawn after it, so it never counts as unchanged.
    std::string footer = std::string(footerLeft ? footerLeft : "") + "\n" + (footerRight ? footerRight : "");
    size_t from = first;
    bool patch = shownValid_ && !turned && top_ == shownTop_;
    if (patch) {
        from = lines.size() - 1;
        for (size_t k = first; k + 1 < lines.size(); k++) {
            const Line &now = lines[k];
            bool same = k + 1 < shownLines_.size() && shownLines_[k].start == now.start && shownLines_[k].end == now.end &&
                        now.end <= shownText_.size() &&
                        shownText_.compare(now.start, now.end - now.start, text, now.start, now.end - now.start) == 0;
            if (!same) {
                from = k;
                break;
            }
        }
    }

    int32_t y = layout_.marginTop;
    for (size_t k = first; k < from; k++)
        y += lineHeight(lines[k]);
    if (patch) {
        clearRows(y, layout_.textBottom);
    } else {
        display_.clearDisplay();
    }

    int32_t cursorX = layout_.marginX;
    int32_t cursorY = y;
    for (size_t k = from; k < lines.size(); k++) {
        cursorX = drawText(display_, body_, layout_.marginX, y + bodyAscent_, s, lines[k].start, lines[k].end);
        cursorY = y;
        y += lineHeight(lines[k]);
    }

    // Cursor: a slim bar just after the last character.
    const int16_t size = body_.yAdvance * 2 / 3; // font size in px (line spacing is 1.5)
    display_.fillRect(cursorX + 4, cursorY + size * 15 / 100, 3, size * 90 / 100, BLACK);

    if (!patch || footer != shownFooter_) {
        if (patch)
            clearRows(layout_.textBottom, display_.height());
        const int32_t baseline = layout_.footerTop + smallAscent_;
        if (footerLeft && *footerLeft)
            drawText(display_, small_, layout_.marginX, baseline, footerLeft, 0, strlen(footerLeft));
        if (footerRight && *footerRight) {
            size_t len = strlen(footerRight);
            int32_t w = measure(small_, footerRight, 0, len);
            drawText(display_, small_, display_.width() - layout_.marginX - w, baseline, footerRight, 0, len);
        }
    }

    shownValid_ = true;
    shownTop_ = top_;
    shownLines_ = lines;
    shownText_ = text;
    shownFooter_ = footer;
    return turned;
}

} // namespace tw
