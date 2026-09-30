// The writing screen: the newest lines of the document, a cursor, and an optional
// one-line footer for notices.
#pragma once

#include <Inkplate.h>
#include <string>
#include <vector>

namespace tw {

struct Layout {
    int16_t marginX = 64;
    int16_t marginTop = 64;
    int16_t textBottom = 720 - 72; // text stops above the footer line
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
    // footer may be null. Returns true when the page turned, so the caller should
    // do a full refresh.
    bool render(const std::string &text, const char *footer);

    // Starts the next render at the top of the text (for a new document).
    void reset() { top_ = 0; }

    // Starts the next render on a page that ends with the end of the text (for
    // opening an existing document).
    void showEnd(const std::string &text);

    // The lines that fit on screen, ending with the line that holds the end of the text.
    std::vector<Line> visibleLines(const std::string &text) const;

  private:
    void wrapParagraph(const std::string &text, size_t start, size_t end, std::vector<Line> &out) const;
    std::vector<Line> linesFrom(const std::string &text, size_t top) const;
    int32_t height(const std::vector<Line> &lines) const;
    int16_t lineHeight(const Line &line) const;

    Inkplate &display_;
    const GFXfont &body_;
    const GFXfont &small_;
    int16_t bodyAscent_;
    int16_t smallAscent_;
    Layout layout_;
    size_t top_ = 0; // byte offset of the first line on the page
};

} // namespace tw
