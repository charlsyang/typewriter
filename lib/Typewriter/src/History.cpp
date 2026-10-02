#include "History.h"

namespace tw {

namespace {

const uint32_t PAUSE_MS = 1500; // a pause this long starts a new step
const size_t MAX_STEPS = 200;

bool isSpace(char c) {
    return c == ' ' || c == '\n' || c == '\t';
}

bool endsWith(const std::string &s, const std::string &suffix) {
    return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

} // namespace

void History::push(std::vector<Step> &stack, Step step) {
    stack.push_back(std::move(step));
    if (stack.size() > MAX_STEPS)
        stack.erase(stack.begin());
}

void History::typed(const std::string &text, uint32_t now) {
    if (text.empty())
        return;
    redo_.clear();
    if (!undo_.empty()) {
        Step &last = undo_.back();
        // Keep adding to the current word; a new word (after a space) or a
        // pause starts a new step.
        bool newWord = isSpace(last.text.back()) && !isSpace(text[0]);
        if (last.typed && now - last.at < PAUSE_MS && !newWord) {
            last.text += text;
            last.at = now;
            return;
        }
    }
    push(undo_, {true, text, now});
}

void History::deleted(const std::string &text, uint32_t now) {
    if (text.empty())
        return;
    redo_.clear();
    if (!undo_.empty()) {
        Step &last = undo_.back();
        if (!last.typed && now - last.at < PAUSE_MS) {
            last.text.insert(0, text); // later deletions come from earlier in the text
            last.at = now;
            return;
        }
    }
    push(undo_, {false, text, now});
}

bool History::undo(std::string &doc) {
    if (undo_.empty())
        return false;
    Step step = undo_.back();
    undo_.pop_back();
    if (step.typed) {
        if (!endsWith(doc, step.text)) {
            clear();
            return false;
        }
        doc.resize(doc.size() - step.text.size());
    } else {
        doc += step.text;
    }
    step.at = 0; // never merge into a redone step
    push(redo_, step);
    return true;
}

bool History::redo(std::string &doc) {
    if (redo_.empty())
        return false;
    Step step = redo_.back();
    redo_.pop_back();
    if (step.typed) {
        doc += step.text;
    } else {
        if (!endsWith(doc, step.text)) {
            clear();
            return false;
        }
        doc.resize(doc.size() - step.text.size());
    }
    step.at = 0;
    undo_.push_back(step);
    return true;
}

void History::clear() {
    undo_.clear();
    redo_.clear();
}

} // namespace tw
