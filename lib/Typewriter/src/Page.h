// The writing screen: the newest lines of the document, a cursor, and an optional
// one-line footer for notices.
#pragma once

#include <Inkplate.h>
#include <string>
#include <vector>

namespace tw {

struct Layout {
    int16_t marginX = 64;
    // A full page (10 lines) sits centered: ~71px of white above the ascenders
    // and below the descenders, with the footer, when shown, in the bottom gap.
    // Room for exactly 10 lines, so blank (half-height) lines can't push a page lower.
    int16_t marginTop = 60;
    int16_t textBottom = 60 + 10 * 60; // text stops above the footer line
    int16_t footerTop = 720 - 44;  // notices and the Cmd+I status, when shown
    size_t carryLines = 2; // lines repeated at the top after a page turn
};

// One wrapped line: a byte range of the document. Empty lines draw at half height.
struct Line {
    size_t start;
    size_t end;
};

class Page {
  public:
    Page(Inkplate &display, const GFXfont &body, int16_t bodyAscent, const GFXfont &small, int16_t smallAscent,
         Layout layout = Layout());

    // Draws the whole screen into the frame buffer; the caller refreshes the panel.
    // The footer has a left and a right-aligned part; either may be null. Returns
    // true when the page turned, so the caller should do a full refresh.
    bool render(const std::string &text, const char *footerLeft, const char *footerRight = nullptr);

    // Starts the next render at the top of the text (for a new document).
    void reset() {
        top_ = 0;
        shownValid_ = false;
    }

    // The next render redraws everything: call after drawing anything else.
    void invalidate() { shownValid_ = false; }

    // Starts the next render on a page that ends with the end of the text (for
    // opening an existing document).
    void showEnd(const std::string &text);

    // The lines that fit on screen, ending with the line that holds the end of the text.
    std::vector<Line> visibleLines(const std::string &text) const;

    // Reading back: the document split into pages from the start, each repeating
    // the last carryLines of the one before, up to the page you're writing on.
    struct Reading {
        std::vector<Line> lines;  // the whole document, wrapped
        std::vector<size_t> starts; // first line of each page before the writing page
    };
    Reading readingPages(const std::string &text) const;

    // Draws one reading page (no cursor). The caller does a full refresh.
    void renderReading(const std::string &text, const Reading &reading, size_t page, const char *footerLeft,
                       const char *footerRight);

  private:
    void wrapParagraph(const std::string &text, size_t start, size_t end, std::vector<Line> &out) const;
    std::vector<Line> linesFrom(const std::string &text, size_t top) const;
    int32_t height(const std::vector<Line> &lines) const;
    int16_t lineHeight(const Line &line) const;
    size_t pageEnd(const std::vector<Line> &lines, size_t start) const; // one past the last line that fits
    void clearRows(int32_t y0, int32_t y1);

    Inkplate &display_;
    const GFXfont &body_;
    const GFXfont &small_;
    int16_t bodyAscent_;
    int16_t smallAscent_;
    Layout layout_;
    size_t top_ = 0; // byte offset of the first line on the page

    // What the frame buffer holds from the last render, so the next one only
    // redraws what changed (usually the line being typed).
    bool shownValid_ = false;
    size_t shownTop_ = 0;
    std::vector<Line> shownLines_;
    std::string shownText_;
    std::string shownFooter_;
};

} // namespace tw
