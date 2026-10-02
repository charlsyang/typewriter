// Undo and redo for the typewriter. Every edit happens at the end of the text,
// so the history is a simple list of "typed this" and "deleted this" steps.
//
// Typing is grouped by word (and by pauses); a run of Backspaces is one step.
#pragma once

#include <stdint.h>
#include <string>
#include <vector>

namespace tw {

class History {
  public:
    // text was added to the end.
    void typed(const std::string &text, uint32_t now);
    // text was removed from the end (in document order).
    void deleted(const std::string &text, uint32_t now);

    // Undo or redo one step on doc. Returns false if there was nothing to do
    // (or the text no longer matches, in which case the history is cleared).
    bool undo(std::string &doc);
    bool redo(std::string &doc);

    void clear();

  private:
    struct Step {
        bool typed;
        std::string text;
        uint32_t at; // millis() of the last change in this step
    };
    void push(std::vector<Step> &stack, Step step);

    std::vector<Step> undo_, redo_;
};

} // namespace tw
