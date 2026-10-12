// The highlight files on the SD card and how a Readwise update changes them.
// Plain C++, tested on the Mac (test/highlight_store_test.cpp). The rules match
// tools/readwise_seed.py, which writes the first copy.
//
//   highlights.jsonl  {"id":…,"text":…,"title":…,"author":…,"fav":…} per line
//   index.bin         8 bytes per line: byte offset (uint32 LE), favorite, 3 zero bytes
//   state.json        {"version":1,"syncedAt":"…Z","count":…,"favorites":…}
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace tw {
namespace highlights {

struct Record {
    uint32_t id = 0;
    std::string text, title, author;
    bool favorite = false;
};

// One highlight from Readwise's export, with what we need to decide on it.
struct Incoming {
    uint32_t id = 0;
    std::string text, title, author;
    bool deleted = false, discarded = false, favorite = false;
    bool sourceDeleted = false, supplemental = false, sourceFavorite = false; // the book or article
    std::vector<std::string> tags;                                           // the highlight's own
};

// Whether a highlight belongs on the card. Left out: deleted or discarded ones,
// empty ones, Readwise supplementals, Ghostreader's AI highlights, and any with
// Chinese, Japanese or Korean text in it, its title or author (the fonts are
// Latin only).
bool keep(const Incoming &h);

// The record for a highlight that's kept: a favorite if marked so, or if its
// source carries the "favorite" tag.
Record toRecord(const Incoming &h);

// True if s has Chinese, Japanese or Korean characters.
bool hasCjk(const std::string &s);

// One line of highlights.jsonl (without the newline).
std::string toLine(const Record &r);

// The id and favorite flag of a highlights.jsonl line, without parsing the rest.
bool readLine(const std::string &line, uint32_t &id, bool &favorite);

struct Counts {
    uint32_t highlights = 0, favorites = 0;
};

// Rewrites highlights.jsonl with changes applied: next() yields the old lines
// one at a time (false at the end); every old line whose id is in changes is
// dropped, and then the changes that are kept (Record id != 0) are added at the
// end. write() receives each new line and whether it's a favorite.
Counts merge(const std::function<bool(std::string &)> &next, const std::map<uint32_t, Record> &changes,
             const std::function<bool(const std::string &, bool)> &write);

// 8-byte index.bin entry for a line at offset.
void indexEntry(uint32_t offset, bool favorite, uint8_t out[8]);

// "2026-10-11T21:14:45Z" <-> Unix time (UTC). 0 if unreadable.
uint32_t parseIsoTime(const std::string &s);
std::string isoTime(uint32_t t);

} // namespace highlights
} // namespace tw
