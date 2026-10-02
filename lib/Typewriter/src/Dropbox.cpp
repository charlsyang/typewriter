#include "Dropbox.h"

#include "Text.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <NetworkClientSecure.h>

namespace tw {

namespace {

// JSON string literal for s, with non-ASCII escaped as \uXXXX: HTTP headers
// such as Dropbox-API-Arg must be ASCII.
std::string jsonString(const std::string &s) {
    std::string out = "\"";
    size_t i = 0;
    char buf[16];
    while (i < s.size()) {
        uint32_t cp = nextCodepoint(s.c_str(), s.size(), i);
        if (cp == '"' || cp == '\\') {
            out += '\\';
            out += (char)cp;
        } else if (cp >= 0x20 && cp < 0x7F) {
            out += (char)cp;
        } else if (cp < 0x10000) {
            snprintf(buf, sizeof(buf), "\\u%04x", (unsigned)cp);
            out += buf;
        } else {
            cp -= 0x10000;
            snprintf(buf, sizeof(buf), "\\u%04x\\u%04x", (unsigned)(0xD800 + (cp >> 10)),
                     (unsigned)(0xDC00 + (cp & 0x3FF)));
            out += buf;
        }
    }
    return out + "\"";
}

uint32_t parseTime(const char *iso) {
    // "2026-09-30T05:41:00Z"
    tm t = {};
    if (!iso || sscanf(iso, "%d-%d-%dT%d:%d:%d", &t.tm_year, &t.tm_mon, &t.tm_mday, &t.tm_hour, &t.tm_min,
                       &t.tm_sec) != 6)
        return 0;
    t.tm_year -= 1900;
    t.tm_mon -= 1;
    // Days since the epoch for a UTC date (no time zone involved).
    int y = t.tm_year + 1900, m = t.tm_mon + 1;
    if (m <= 2) {
        y--;
        m += 12;
    }
    int32_t days = 365 * y + y / 4 - y / 100 + y / 400 + (153 * (m - 3) + 2) / 5 + t.tm_mday - 719469;
    return days * 86400 + t.tm_hour * 3600 + t.tm_min * 60 + t.tm_sec;
}

void readEntry(JsonObjectConst o, Dropbox::Entry &e) {
    e.name = o["name"] | "";
    e.path = o["path_display"] | "";
    e.rev = o["rev"] | "";
    e.modified = parseTime(o["server_modified"] | "");
    e.folder = strcmp(o[".tag"] | "", "folder") == 0;
}

} // namespace

Dropbox::Dropbox(const char *appKey, const char *refreshToken) : appKey_(appKey), refreshToken_(refreshToken) {
}

// One secure connection per Dropbox host, kept open between requests (HTTP
// keep-alive): each new TLS handshake costs 2-3 seconds on the ESP32.
NetworkClientSecure &Dropbox::connection(bool content) {
    NetworkClientSecure *&tls = content ? contentTls_ : apiTls_;
    if (!tls) {
        tls = new NetworkClientSecure();
        tls->useBuiltinCACertBundle(); // Mozilla roots bundled with ESP-IDF
    }
    return *tls;
}

std::string Dropbox::tlsError(NetworkClientSecure &tls) {
    char text[96] = "";
    tls.lastError(text, sizeof(text));
    return text;
}

bool Dropbox::ensureToken() {
    if (!accessToken_.empty() && (int32_t)(tokenExpiresAt_ - millis()) > 60000)
        return true;
    String body = String("grant_type=refresh_token&refresh_token=") + refreshToken_.c_str() +
                  "&client_id=" + appKey_.c_str();
    NetworkClientSecure &tls = connection(false);
    int code = -1;
    String response;
    for (int attempt = 0; attempt < 2 && code < 0; attempt++) {
        HTTPClient http;
        http.setReuse(true);
        if (!http.begin(tls, "https://api.dropboxapi.com/oauth2/token"))
            continue;
        http.addHeader("Content-Type", "application/x-www-form-urlencoded");
        code = http.POST(body);
        response = http.getString();
        http.end();
        if (code < 0)
            tls.stop(); // a kept-alive connection the server had closed: retry fresh
    }
    if (code != 200) {
        std::string tlsText = code < 0 ? tlsError(tls) : "";
        lastError_ = "sign-in failed (" + std::to_string(code) + (tlsText.empty() ? "" : ", " + tlsText) + ")";
        return false;
    }
    JsonDocument doc;
    if (deserializeJson(doc, response)) {
        lastError_ = "unreadable sign-in reply";
        return false;
    }
    accessToken_ = doc["access_token"] | "";
    tokenExpiresAt_ = millis() + (uint32_t)(doc["expires_in"] | 3600) * 1000;
    return !accessToken_.empty();
}

// One API call. RPC endpoints (api host) take arg as the JSON body; content
// endpoints take it in the Dropbox-API-Arg header, with body as the file.
Dropbox::Result Dropbox::call(const char *host, const char *endpoint, const std::string &arg,
                              const std::string *body, std::string &response, std::string *apiResult,
                              const char *range) {
    if (!ensureToken())
        return Result::Error;
    bool content = strcmp(host, "content.dropboxapi.com") == 0;
    NetworkClientSecure &tls = connection(content);
    String url = String("https://") + host + "/2/" + endpoint;
    int code = -1;
    for (int attempt = 0; attempt < 2 && code < 0; attempt++) {
        HTTPClient http;
        http.setReuse(true);
        if (!http.begin(tls, url))
            continue;
        http.setTimeout(15000);
        http.addHeader("Authorization", String("Bearer ") + accessToken_.c_str());
        const char *collect[] = {"Dropbox-API-Result"};
        http.collectHeaders(collect, 1);
        if (range)
            http.addHeader("Range", range);
        if (content) {
            http.addHeader("Dropbox-API-Arg", arg.c_str());
            if (body) {
                http.addHeader("Content-Type", "application/octet-stream");
                code = http.POST((uint8_t *)body->data(), body->size());
            } else {
                code = http.POST((uint8_t *)nullptr, 0);
            }
        } else {
            http.addHeader("Content-Type", "application/json");
            code = http.POST((uint8_t *)arg.data(), arg.size());
        }
        response.clear();
        if (code > 0) {
            // getString() handles chunked replies; text files hold no NUL bytes.
            String s = http.getString();
            response.assign(s.c_str(), s.length());
            if (apiResult)
                *apiResult = http.header("Dropbox-API-Result").c_str();
        }
        http.end();
        if (code < 0)
            tls.stop(); // a kept-alive connection the server had closed: retry fresh
    }

    if (code == 200 || code == 206) // 206: partial content, for a Range request
        return Result::Ok;
    if (code == 401)
        accessToken_.clear(); // expired early: sign in again next time
    if (code == 409) {
        // Endpoint-specific errors, e.g. "path/not_found/.." or "path/conflict/file/..".
        if (response.find("not_found") != std::string::npos)
            return Result::NotFound;
        if (response.find("conflict") != std::string::npos)
            return Result::Conflict;
    }
    lastError_ = std::string(endpoint) + " failed (" + std::to_string(code) + ")";
    if (code < 0) {
        std::string tlsText = tlsError(tls);
        if (!tlsText.empty())
            lastError_ += ": " + tlsText;
    } else if (!response.empty()) {
        lastError_ += ": " + response.substr(0, 120);
    }
    return Result::Error;
}

Dropbox::Result Dropbox::metadata(const std::string &path, Entry &out) {
    std::string response;
    Result r = call("api.dropboxapi.com", "files/get_metadata", "{\"path\":" + jsonString(path) + "}", nullptr,
                    response, nullptr);
    if (r != Result::Ok)
        return r;
    JsonDocument doc;
    if (deserializeJson(doc, response))
        return Result::Error;
    readEntry(doc.as<JsonObjectConst>(), out);
    return Result::Ok;
}

Dropbox::Result Dropbox::download(const std::string &path, std::string &data, std::string &rev) {
    std::string apiResult;
    Result r = call("content.dropboxapi.com", "files/download", "{\"path\":" + jsonString(path) + "}", nullptr,
                    data, &apiResult);
    if (r != Result::Ok)
        return r;
    JsonDocument doc;
    if (deserializeJson(doc, apiResult))
        return Result::Error;
    rev = doc["rev"] | "";
    return Result::Ok;
}

Dropbox::Result Dropbox::downloadStart(const std::string &path, size_t maxBytes, std::string &data) {
    std::string range = "bytes=0-" + std::to_string(maxBytes - 1);
    return call("content.dropboxapi.com", "files/download", "{\"path\":" + jsonString(path) + "}", nullptr, data,
                nullptr, range.c_str());
}

Dropbox::Result Dropbox::remove(const std::string &path, const std::string &rev) {
    std::string response;
    std::string arg = "{\"path\":" + jsonString(path) + ",\"parent_rev\":" + jsonString(rev) + "}";
    return call("api.dropboxapi.com", "files/delete_v2", arg, nullptr, response, nullptr);
}

Dropbox::Result Dropbox::upload(const std::string &path, const std::string &data, const std::string &rev,
                                std::string &newRev) {
    std::string mode = rev.empty() ? "\"add\"" : "{\".tag\":\"update\",\"update\":" + jsonString(rev) + "}";
    std::string arg = "{\"path\":" + jsonString(path) + ",\"mode\":" + mode + ",\"autorename\":false,\"mute\":true}";
    std::string response;
    Result r = call("content.dropboxapi.com", "files/upload", arg, &data, response, nullptr);
    if (r != Result::Ok)
        return r;
    JsonDocument doc;
    if (deserializeJson(doc, response))
        return Result::Error;
    newRev = doc["rev"] | "";
    return Result::Ok;
}

Dropbox::Result Dropbox::uploadCopy(const std::string &path, const std::string &data, std::string &savedPath) {
    std::string arg = "{\"path\":" + jsonString(path) + ",\"mode\":\"add\",\"autorename\":true,\"mute\":true}";
    std::string response;
    Result r = call("content.dropboxapi.com", "files/upload", arg, &data, response, nullptr);
    if (r != Result::Ok)
        return r;
    JsonDocument doc;
    if (!deserializeJson(doc, response))
        savedPath = doc["path_display"] | path.c_str();
    return Result::Ok;
}

Dropbox::Result Dropbox::list(const std::string &folder, std::vector<Entry> &out) {
    out.clear();
    std::string response;
    std::string arg = "{\"path\":" + jsonString(folder) + ",\"limit\":2000}";
    const char *endpoint = "files/list_folder";
    while (true) {
        Result r = call("api.dropboxapi.com", endpoint, arg, nullptr, response, nullptr);
        if (r != Result::Ok)
            return r;
        JsonDocument doc;
        if (deserializeJson(doc, response))
            return Result::Error;
        for (JsonObjectConst e : doc["entries"].as<JsonArrayConst>()) {
            Entry entry;
            readEntry(e, entry);
            out.push_back(entry);
        }
        if (!(doc["has_more"] | false))
            return Result::Ok;
        arg = std::string("{\"cursor\":") + jsonString(doc["cursor"] | "") + "}";
        endpoint = "files/list_folder/continue";
    }
}

} // namespace tw
