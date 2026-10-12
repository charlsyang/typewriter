// Host tests for lib/Typewriter/src/HighlightStore.cpp. Run: test/run.sh
#include "HighlightStore.h"

#include <cstdio>

using namespace tw::highlights;

static int failures = 0;
#define EXPECT(cond)                                                                                                   \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            printf("  FAIL line %d: %s\n", __LINE__, #cond);                                                          \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

static Incoming incoming(uint32_t id, const std::string &text) {
    Incoming h;
    h.id = id;
    h.text = text;
    h.title = "A Title";
    h.author = "Someone";
    return h;
}

int main() {
    // keep(): the seed script's rules
    EXPECT(keep(incoming(1, "Plain words.")));
    Incoming h = incoming(1, "Plain words.");
    h.deleted = true;
    EXPECT(!keep(h));
    h = incoming(1, "x");
    h.discarded = true;
    EXPECT(!keep(h));
    h = incoming(1, "x");
    h.supplemental = true;
    EXPECT(!keep(h));
    h = incoming(1, "x");
    h.sourceDeleted = true;
    EXPECT(!keep(h));
    EXPECT(!keep(incoming(1, "  \n ")));
    h = incoming(1, "x");
    h.tags = {"craft", "\xF0\x9F\x91\xBB ai highlighted"};
    EXPECT(!keep(h));
    EXPECT(!keep(incoming(1, "\xE7\x9A\x84 Chinese")));          // 的
    h = incoming(1, "Latin text");
    h.title = "\xE5\x88\xB6\xE9\x80\xA0"; // 制造: a Chinese title
    EXPECT(!keep(h));
    EXPECT(keep(incoming(1, "Curly \xE2\x80\x9Cquotes\xE2\x80\x9D \xE2\x80\x94 and \xC3\xA9")));
    EXPECT(!hasCjk("\xE2\x80\x94\xC3\xA9 caf\xC3\xA9"));
    EXPECT(hasCjk("\xEF\xBC\x8C")); // fullwidth comma

    // toRecord: favorite from the highlight or its source's tag
    h = incoming(5, "x");
    EXPECT(!toRecord(h).favorite);
    h.sourceFavorite = true;
    EXPECT(toRecord(h).favorite && toRecord(h).id == 5);

    // toLine / readLine
    Record r;
    r.id = 600371007;
    r.text = "Say \"hi\"\\ now\nnext";
    r.title = "T";
    r.author = "A";
    r.favorite = true;
    std::string line = toLine(r);
    EXPECT(line == "{\"id\":600371007,\"text\":\"Say \\\"hi\\\"\\\\ now\\nnext\",\"title\":\"T\",\"author\":\"A\",\"fav\":true}");
    uint32_t id = 0;
    bool fav = false;
    EXPECT(readLine(line, id, fav) && id == 600371007 && fav);
    EXPECT(readLine("{\"id\":7,\"text\":\"x\",\"title\":\"\",\"author\":\"\",\"fav\":false}", id, fav) && id == 7 && !fav);
    EXPECT(!readLine("garbage", id, fav));

    // merge: changed lines leave their place and come back at the end; removed ones go
    std::vector<std::string> old = {
        "{\"id\":1,\"text\":\"one\",\"title\":\"\",\"author\":\"\",\"fav\":false}",
        "{\"id\":2,\"text\":\"two\",\"title\":\"\",\"author\":\"\",\"fav\":true}",
        "{\"id\":3,\"text\":\"three\",\"title\":\"\",\"author\":\"\",\"fav\":false}",
    };
    size_t pos = 0;
    auto next = [&](std::string &l) {
        if (pos >= old.size())
            return false;
        l = old[pos++];
        return true;
    };
    std::map<uint32_t, Record> changes;
    Record edited;
    edited.id = 3;
    edited.text = "three, edited";
    changes[3] = edited;
    changes[2] = Record(); // removed: id 0
    Record added;
    added.id = 9;
    added.text = "nine";
    added.favorite = true;
    changes[9] = added;
    std::vector<std::string> out;
    std::vector<bool> favs;
    Counts c = merge(next, changes, [&](const std::string &l, bool f) {
        out.push_back(l);
        favs.push_back(f);
        return true;
    });
    EXPECT(c.highlights == 3 && c.favorites == 1);
    EXPECT(out.size() == 3 && out[0] == old[0]);
    EXPECT(readLine(out[1], id, fav) && id == 3 && out[1].find("three, edited") != std::string::npos);
    EXPECT(readLine(out[2], id, fav) && id == 9 && fav && favs[2]);

    // index entries
    uint8_t e[8];
    indexEntry(0x01020304, true, e);
    EXPECT(e[0] == 4 && e[1] == 3 && e[2] == 2 && e[3] == 1 && e[4] == 1 && e[7] == 0);

    // ISO times
    EXPECT(parseIsoTime("1970-01-01T00:00:00Z") == 0);
    EXPECT(parseIsoTime("2026-10-11T21:14:45Z") == 1791753285u);
    EXPECT(isoTime(1791753285u) == "2026-10-11T21:14:45Z");
    EXPECT(isoTime(parseIsoTime("2024-02-29T23:59:59Z")) == "2024-02-29T23:59:59Z");
    EXPECT(parseIsoTime("not a time") == 0);

    printf(failures ? "%d FAILED\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}
