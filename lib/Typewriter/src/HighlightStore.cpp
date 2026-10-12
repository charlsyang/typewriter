#include "HighlightStore.h"

#include <cstdio>
#include <cstdlib>

namespace tw {
namespace highlights {

static const char *AI_TAG = "\xF0\x9F\x91\xBB ai highlighted"; // 👻 ai highlighted, Reader's Ghostreader
static const char *FAVORITE_TAG = "favorite";

bool hasCjk(const std::string &s) {
    for (size_t i = 0; i < s.size();) {
        uint8_t b = s[i];
        if (b < 0x80) {
            i++;
            continue;
        }
        int extra = (b & 0xE0) == 0xC0 ? 1 : (b & 0xF0) == 0xE0 ? 2 : (b & 0xF8) == 0xF0 ? 3 : 0;
        if (!extra || i + extra >= s.size()) { // not a lead byte, or cut short
            i++;
            continue;
        }
        uint32_t cp = b & (0x3F >> extra);
        for (int k = 1; k <= extra; k++)
            cp = cp << 6 | (s[i + k] & 0x3F);
        i += extra + 1;
        if ((cp >= 0x2E80 && cp <= 0x9FFF) || (cp >= 0xAC00 && cp <= 0xD7AF) || (cp >= 0xF900 && cp <= 0xFAFF) ||
            (cp >= 0xFE30 && cp <= 0xFE4F) || (cp >= 0xFF00 && cp <= 0xFFEF))
            return true;
    }
    return false;
}

static bool blank(const std::string &s) {
    return s.find_first_not_of(" \t\r\n") == std::string::npos;
}

bool keep(const Incoming &h) {
    if (h.deleted || h.discarded || h.sourceDeleted || h.supplemental || blank(h.text))
        return false;
    for (const std::string &t : h.tags) {
        if (t == AI_TAG)
            return false;
    }
    return !hasCjk(h.text + h.title + h.author);
}

Record toRecord(const Incoming &h) {
    Record r;
    r.id = h.id;
    r.text = h.text;
    r.title = h.title;
    r.author = h.author;
    r.favorite = h.favorite || h.sourceFavorite;
    return r;
}

static void appendJsonString(std::string &out, const std::string &s) {
    out += '"';
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') {
            out += '\\';
            out += c;
        } else if (c == '\n') {
            out += "\\n";
        } else if (c == '\r') {
            out += "\\r";
        } else if (c == '\t') {
            out += "\\t";
        } else if (c < 0x20) {
            char buf[8];
            snprintf(buf, sizeof buf, "\\u%04x", c);
            out += buf;
        } else {
            out += c; // UTF-8 passes through, as the seed script writes it
        }
    }
    out += '"';
}

std::string toLine(const Record &r) {
    std::string out = "{\"id\":" + std::to_string(r.id) + ",\"text\":";
    appendJsonString(out, r.text);
    out += ",\"title\":";
    appendJsonString(out, r.title);
    out += ",\"author\":";
    appendJsonString(out, r.author);
    out += r.favorite ? ",\"fav\":true}" : ",\"fav\":false}";
    return out;
}

bool readLine(const std::string &line, uint32_t &id, bool &favorite) {
    if (line.compare(0, 6, "{\"id\":") != 0)
        return false;
    char *end = nullptr;
    unsigned long v = strtoul(line.c_str() + 6, &end, 10);
    if (end == line.c_str() + 6)
        return false;
    id = v;
    favorite = line.size() >= 11 && line.compare(line.size() - 11, 11, "\"fav\":true}") == 0;
    return true;
}

Counts merge(const std::function<bool(std::string &)> &next, const std::map<uint32_t, Record> &changes,
             const std::function<bool(const std::string &, bool)> &write) {
    Counts counts;
    std::string line;
    while (next(line)) {
        uint32_t id;
        bool favorite;
        if (!readLine(line, id, favorite) || changes.count(id))
            continue; // replaced or removed (or unreadable)
        if (!write(line, favorite))
            return Counts();
        counts.highlights++;
        counts.favorites += favorite;
    }
    for (const auto &c : changes) {
        if (!c.second.id)
            continue; // removed
        if (!write(toLine(c.second), c.second.favorite))
            return Counts();
        counts.highlights++;
        counts.favorites += c.second.favorite;
    }
    return counts;
}

void indexEntry(uint32_t offset, bool favorite, uint8_t out[8]) {
    out[0] = offset;
    out[1] = offset >> 8;
    out[2] = offset >> 16;
    out[3] = offset >> 24;
    out[4] = favorite;
    out[5] = out[6] = out[7] = 0;
}

// Days since 1970-01-01 for a civil date (Howard Hinnant's algorithm).
static int64_t daysFromCivil(int64_t y, unsigned m, unsigned d) {
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}

uint32_t parseIsoTime(const std::string &s) {
    int y, mo, d, h, mi, sec;
    if (sscanf(s.c_str(), "%4d-%2d-%2dT%2d:%2d:%2d", &y, &mo, &d, &h, &mi, &sec) != 6)
        return 0;
    if (mo < 1 || mo > 12 || d < 1 || d > 31)
        return 0;
    return (uint32_t)(daysFromCivil(y, mo, d) * 86400 + h * 3600 + mi * 60 + sec);
}

std::string isoTime(uint32_t t) {
    int64_t days = t / 86400, rem = t % 86400;
    // civil from days (inverse of the above)
    int64_t z = days + 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = (unsigned)(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t y = (int64_t)yoe + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    const unsigned d = doy - (153 * mp + 2) / 5 + 1;
    const unsigned m = mp + (mp < 10 ? 3 : -9);
    y += m <= 2;
    char buf[24];
    snprintf(buf, sizeof buf, "%04d-%02u-%02uT%02d:%02d:%02dZ", (int)y, m, d, (int)(rem / 3600), (int)(rem / 60 % 60),
             (int)(rem % 60));
    return buf;
}

} // namespace highlights
} // namespace tw
