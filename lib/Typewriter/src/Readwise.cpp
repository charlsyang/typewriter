#include "Readwise.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <NetworkClientSecure.h>
#include <algorithm>
#include <esp_heap_caps.h>

namespace tw {

namespace {

// Export pages can be large; their JSON goes to PSRAM, not the scarce internal heap.
struct SpiRamAllocator : ArduinoJson::Allocator {
    void *allocate(size_t size) override { return heap_caps_malloc(size, MALLOC_CAP_SPIRAM); }
    void deallocate(void *p) override { heap_caps_free(p); }
    void *reallocate(void *p, size_t size) override { return heap_caps_realloc(p, size, MALLOC_CAP_SPIRAM); }
};
SpiRamAllocator spiRam;

std::string urlEncode(const std::string &s) {
    std::string out;
    char buf[4];
    for (unsigned char c : s) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out += c;
        } else {
            snprintf(buf, sizeof buf, "%%%02X", c);
            out += buf;
        }
    }
    return out;
}

bool hasTag(JsonArrayConst tags, const char *name) {
    for (JsonObjectConst t : tags) {
        if (strcasecmp(t["name"] | "", name) == 0)
            return true;
    }
    return false;
}

} // namespace

bool Readwise::exportPage(const std::string &after, std::string &cursor,
                          const std::function<void(const highlights::Incoming &)> &onHighlight) {
    std::string url = "https://readwise.io/api/v2/export/?includeDeleted=true";
    if (!after.empty())
        url += "&updatedAfter=" + urlEncode(after);
    if (!cursor.empty())
        url += "&pageCursor=" + urlEncode(cursor);

    NetworkClientSecure tls;
    tls.useBuiltinCACertBundle(); // Mozilla roots bundled with ESP-IDF
    HTTPClient http;
    http.useHTTP10(true); // no chunked encoding, so the JSON can be parsed straight off the stream
    const char *keep[] = {"Retry-After"};
    http.collectHeaders(keep, 1);
    if (!http.begin(tls, url.c_str())) {
        lastError_ = "can't reach readwise.io";
        return false;
    }
    http.addHeader("Authorization", ("Token " + token_).c_str());
    int code = http.GET();
    if (code != 200) {
        lastError_ = "HTTP " + std::to_string(code);
        if (code == 429)
            lastError_ += ", retry after " + std::string(http.header("Retry-After").c_str()) + " s";
        if (code == 401)
            lastError_ += " (check READWISE_TOKEN)";
        http.end();
        return false;
    }

    // Only the fields we use, so a big page costs little memory.
    JsonDocument filter;
    filter["nextPageCursor"] = true;
    JsonObject book = filter["results"][0].to<JsonObject>();
    for (const char *f : {"is_deleted", "title", "readable_title", "author", "category"})
        book[f] = true;
    book["book_tags"][0]["name"] = true;
    JsonObject hl = book["highlights"][0].to<JsonObject>();
    for (const char *f : {"id", "text", "is_deleted", "is_discard", "is_favorite"})
        hl[f] = true;
    hl["tags"][0]["name"] = true;

    // The whole reply first (no Content-Length: it ends when Readwise closes the
    // connection), into PSRAM; a page is at most a few hundred KB.
    size_t size = 0, capacity = 64 * 1024;
    char *body = (char *)heap_caps_malloc(capacity, MALLOC_CAP_SPIRAM);
    NetworkClient &stream = http.getStream();
    uint32_t lastDataAt = millis();
    bool timedOut = false;
    while (body) {
        int n = stream.available();
        if (n > 0) {
            if (size + n > capacity) {
                capacity = std::max(capacity * 2, size + n);
                char *bigger = (char *)heap_caps_realloc(body, capacity, MALLOC_CAP_SPIRAM);
                if (!bigger) {
                    heap_caps_free(body);
                    body = nullptr;
                    break;
                }
                body = bigger;
            }
            size += stream.read((uint8_t *)body + size, n);
            lastDataAt = millis();
        } else if (!stream.connected()) {
            break;
        } else if (millis() - lastDataAt > 15000) {
            timedOut = true;
            break;
        } else {
            delay(5);
        }
    }
    http.end();
    if (!body) {
        lastError_ = "out of memory for the reply";
        return false;
    }
    lastBytes_ = size;
    JsonDocument doc(&spiRam);
    DeserializationError err = deserializeJson(doc, body, size, DeserializationOption::Filter(filter));
    heap_caps_free(body);
    if (err) {
        lastError_ = std::string("unreadable reply (") + err.c_str() + ", " + std::to_string(size) + " bytes" +
                     (timedOut ? ", timed out" : "") + ")";
        return false;
    }
    for (JsonObjectConst b : doc["results"].as<JsonArrayConst>()) {
        highlights::Incoming h;
        const char *readable = b["readable_title"] | "";
        h.title = *readable ? readable : (b["title"] | "");
        h.author = b["author"] | "";
        h.sourceDeleted = b["is_deleted"] | false;
        h.supplemental = strcmp(b["category"] | "", "supplementals") == 0;
        h.sourceFavorite = hasTag(b["book_tags"], "favorite");
        for (JsonObjectConst x : b["highlights"].as<JsonArrayConst>()) {
            h.id = x["id"] | 0u;
            h.text = x["text"] | "";
            h.deleted = x["is_deleted"] | false;
            h.discarded = x["is_discard"] | false;
            h.favorite = x["is_favorite"] | false;
            h.tags.clear();
            for (JsonObjectConst t : x["tags"].as<JsonArrayConst>())
                h.tags.push_back(t["name"] | "");
            if (h.id)
                onHighlight(h);
        }
    }
    JsonVariantConst next = doc["nextPageCursor"];
    cursor = next.isNull() ? "" : next.is<const char *>() ? next.as<std::string>() : std::to_string(next.as<long long>());
    return true;
}

} // namespace tw
