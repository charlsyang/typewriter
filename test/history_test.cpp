// Host tests for lib/Typewriter/src/History.cpp. Run: test/run.sh
#include "History.h"

#include <cstdio>

using namespace tw;

static int failures = 0;
#define EXPECT(cond)                                                                                                   \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            printf("  FAIL line %d: %s\n", __LINE__, #cond);                                                          \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

// Simulates the firmware: type characters one at a time, or Backspace.
struct Doc {
    std::string text;
    History h;
    uint32_t t = 1000;
    void type(const std::string &s, uint32_t gap = 100) {
        for (char c : s) {
            t += gap;
            text += c;
            h.typed(std::string(1, c), t);
        }
    }
    void backspace(int n, uint32_t gap = 100) {
        for (int i = 0; i < n; i++) {
            t += gap;
            std::string gone(1, text.back());
            text.pop_back();
            h.deleted(gone, t);
        }
    }
};

int main() {
    {
        puts("undo a run of Backspaces restores the text");
        Doc d;
        d.type("Hello world");
        d.t += 2000;
        d.backspace(5);
        EXPECT(d.text == "Hello ");
        EXPECT(d.h.undo(d.text) && d.text == "Hello world");
    }
    {
        puts("undo typing removes the last word, then the one before");
        Doc d;
        d.type("one two three");
        EXPECT(d.h.undo(d.text) && d.text == "one two ");
        EXPECT(d.h.undo(d.text) && d.text == "one ");
    }
    {
        puts("deleted, then typed: undo the typing first, then the deletion");
        Doc d;
        d.type("foo");
        d.t += 2000;
        d.backspace(3);
        d.t += 2000;
        d.type("bar");
        EXPECT(d.h.undo(d.text) && d.text == "");
        EXPECT(d.h.undo(d.text) && d.text == "foo");
    }
    {
        puts("redo puts it back; new typing clears redo");
        Doc d;
        d.type("keep this");
        d.t += 2000;
        d.backspace(4);
        d.h.undo(d.text);
        EXPECT(d.text == "keep this");
        EXPECT(d.h.redo(d.text) && d.text == "keep ");
        d.h.undo(d.text);
        d.type("!");
        EXPECT(!d.h.redo(d.text));
    }
    {
        puts("a pause starts a new step");
        Doc d;
        d.type("abc");
        d.type("def", 2000);
        EXPECT(d.h.undo(d.text) && d.text == "abcde");
    }
    {
        puts("text changed elsewhere (e.g. by sync): undo refuses instead of corrupting");
        Doc d;
        d.type("mine");
        d.text = "replaced by sync";
        EXPECT(!d.h.undo(d.text) && d.text == "replaced by sync");
    }
    {
        puts("multi-byte characters undo whole");
        Doc d;
        d.text = "";
        d.t += 100;
        d.text += "—";
        d.h.typed("—", d.t);
        EXPECT(d.h.undo(d.text) && d.text.empty());
    }
    printf(failures ? "\n%d FAILED\n" : "\nall passed\n", failures);
    return failures ? 1 : 0;
}
