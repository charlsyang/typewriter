// Host tests for lib/Typewriter/src/Highlight.cpp. Run: test/run.sh
#include "Highlight.h"

#include <cstdio>

using namespace tw::highlight;

static int failures = 0;
#define EXPECT(cond)                                                                                                   \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            printf("  FAIL line %d: %s\n", __LINE__, #cond);                                                          \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

// A font where every code point is 10 px wide.
static int32_t width16(const std::string &s, size_t start, size_t end) {
    int32_t n = 0;
    for (size_t i = start; i < end; i++)
        n += ((unsigned char)s[i] & 0xC0) != 0x80;
    return n * 160;
}
static const Size TEN = {10, 14, width16};

static bool latin(uint32_t cp) { return cp < 0x2000 || (cp >= 0x2010 && cp <= 0x2026) || (cp >= 0xFB00 && cp <= 0xFB04); }

int main() {
    // clean: Markdown links and emphasis, spaces, blank lines, characters the font lacks
    EXPECT(clean("Read [this essay](https://x.com/a) now", latin) == "Read this essay now");
    EXPECT(clean("![alt words](img.png) after", latin) == "alt words after");
    EXPECT(clean("a **bold** and *soft* and __under__", latin) == "a bold and soft and under");
    EXPECT(clean("  one   two\t three  ", latin) == "one two three");
    EXPECT(clean("para one\n\n\npara two\r\n", latin) == "para one > para two");
    EXPECT(clean("This is one line.\nThis is second line", latin) == "This is one line. > This is second line");
    EXPECT(clean("trailing space \n next", latin) == "trailing space > next");
    EXPECT(clean("dove \xF0\x9F\x95\x8A peace", latin) == "dove peace");          // emoji dropped
    EXPECT(clean("hair\xE2\x80\x8A" "space", latin) == "hair space");             // U+200A -> space
    EXPECT(clean("curly \xE2\x80\x9Cquotes\xE2\x80\x9D", latin) == "curly \xE2\x80\x9Cquotes\xE2\x80\x9D");
    EXPECT(clean("snake_case stays", latin) == "snake_case stays");
    EXPECT(clean("[not a link] (really)", latin) == "[not a link] (really)");

    // wrap: greedy, at most 10 characters (100 px) per line
    auto lines = wrap("aaa bbb ccc ddd", TEN, 100);
    EXPECT(lines.size() == 2 && lines[0] == "aaa bbb" && lines[1] == "ccc ddd");
    lines = wrap("first\nsecond line here", TEN, 100);
    EXPECT(lines.size() == 3 && lines[0] == "first" && lines[1] == "second" && lines[2] == "line here");
    lines = wrap("supercalifragilistic word", TEN, 100);
    EXPECT(lines.size() == 2 && lines[0] == "supercalifragilistic" && lines[1] == "word");
    lines = wrap("exactly10c", TEN, 100);
    EXPECT(lines.size() == 1);

    // ">" stays on the word before it, in wrap and balance
    for (int balanced = 0; balanced < 2; balanced++) {
        auto ls = balanced ? balance("one line. > next words here", TEN, 100) : wrap("one line. > next words here", TEN, 100);
        for (auto &l : ls)
            EXPECT(l.compare(0, 1, ">") != 0);
    }
    lines = wrap("aaaaaa > b", TEN, 100);
    EXPECT(lines.size() == 1 && lines[0] == "aaaaaa > b");
    lines = wrap("aaaaaaa > bbb", TEN, 100);
    EXPECT(lines.size() == 2 && lines[0] == "aaaaaaa >" && lines[1] == "bbb");

    // balance: never one word alone on the last line, as many lines as wrap
    lines = balance("aaaa bbbb c", TEN, 100); // wrap: "aaaa bbbb" / "c"
    EXPECT(lines.size() == 2 && lines[0] == "aaaa" && lines[1] == "bbbb c");
    lines = balance("aa bb cc dd ee ff gg", TEN, 100); // wrap: "aa bb cc" / "dd ee ff" / "gg"
    EXPECT(lines.size() == 3 && lines[2].find(' ') != std::string::npos);
    // evens out: wrap gives 10 + 2 characters, balance 6 + 6
    lines = balance("aaaaa bbbb cc", TEN, 100);
    EXPECT(lines.size() == 2 && lines[0] == "aaaaa" && lines[1] == "bbbb cc");
    lines = balance("short", TEN, 100);
    EXPECT(lines.size() == 1 && lines[0] == "short");
    // every line still fits; the count matches wrap
    std::string longText = "the quick brown fox jumps over the lazy dog while seven small birds watch from a fence";
    auto greedy = wrap(longText, TEN, 100), even = balance(longText, TEN, 100);
    EXPECT(greedy.size() == even.size());
    for (auto &l : even)
        EXPECT(width16(l, 0, l.size()) <= 100 * 16);
    EXPECT(even.back().find(' ') != std::string::npos);

    // truncate: last line ends with … and still fits; trailing punctuation goes first
    lines = {"one two", "three four", "five six"};
    EXPECT(truncate(lines, 2, TEN, 100));
    EXPECT(lines.size() == 2 && lines[1] == "three\xE2\x80\xA6");
    lines = {"one", "two,", "three"};
    EXPECT(truncate(lines, 2, TEN, 100) && lines[1] == "two\xE2\x80\xA6");
    lines = {"one", "two \xE2\x80\x94", "three"};
    EXPECT(truncate(lines, 2, TEN, 100) && lines[1] == "two\xE2\x80\xA6");
    lines = {"one", "two"};
    EXPECT(!truncate(lines, 2, TEN, 100) && lines.size() == 2);

    // fit: largest size that fits, else the last size cut
    static const Size BIG = {20, 28, [](const std::string &s, size_t a, size_t b) { return 2 * width16(s, a, b); }};
    std::vector<Size> sizes = {BIG, TEN};
    auto room = [](size_t) -> size_t { return 2; };
    Layout l = fit("aa bb", sizes, room, 100);
    EXPECT(l.fits && l.sizeIndex == 0 && l.lines.size() == 1);
    l = fit("aaa bbb ccc ddd", sizes, room, 100);
    EXPECT(l.fits && l.sizeIndex == 1 && l.lines.size() == 2);
    l = fit("aaa bbb ccc ddd eee fff", sizes, room, 100);
    EXPECT(!l.fits);

    // pickIndex: favorites come out 1.5x as often as the others, per highlight
    {
        uint32_t state = 12345; // xorshift: deterministic
        auto rng = [&]() { state ^= state << 13; state ^= state >> 17; state ^= state << 5; return state; };
        auto fav = [](size_t i) { return i % 4 == 0; }; // a quarter are favorites
        size_t favPicks = 0, otherPicks = 0;
        for (int k = 0; k < 200000; k++)
            (fav(pickIndex(1000, fav, rng)) ? favPicks : otherPicks)++;
        double ratio = (favPicks / 250.0) / (otherPicks / 750.0); // per-highlight rate
        EXPECT(ratio > 1.45 && ratio < 1.55);
        EXPECT(pickIndex(0, fav, rng) == 0);
        EXPECT(pickIndex(1, fav, rng) == 0);
    }

    // Recent: remembers the last 32, forgets the oldest
    {
        Recent r;
        r.clear();
        EXPECT(!r.contains(7));
        r.add(7);
        EXPECT(r.contains(7));
        for (uint32_t id = 100; id < 100 + Recent::CAPACITY - 1; id++)
            r.add(id);
        EXPECT(r.contains(7)); // 32 in, still there
        r.add(999);
        EXPECT(!r.contains(7) && r.contains(999) && r.contains(100));
        r.add(1000);
        EXPECT(!r.contains(100) && r.contains(101));
    }

    printf(failures ? "%d FAILED\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}
