// The writing screen: the newest lines of the document, a cursor, and a status bar.
#pragma once

#include <Inkplate.h>
#include <string>
#include <vector>

namespace tw {

struct Layout {
    int16_t marginX = 64;
    int16_t marginTop = 64;
    int16_t textBottom = 720 - 72; // text stops above the status bar
    int16_t statusRuleY = 720 - 56;
    int16_t statusTop = 720 - 44;
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
    void render(const std::string &text, const char *statusLeft, const char *statusRight);

    // The lines that fit on screen, ending with the line that holds the end of the text.
    std::vector<Line> visibleLines(const std::string &text) const;

  private:
    void wrapParagraph(const std::string &text, size_t start, size_t end, std::vector<Line> &out) const;
    int16_t lineHeight(const Line &line) const;

    Inkplate &display_;
    const GFXfont &body_;
    const GFXfont &small_;
    int16_t bodyAscent_;
    int16_t smallAscent_;
    Layout layout_;
};

} // namespace tw
