// A small Dropbox API v2 client: the handful of calls the typewriter needs.
//
// Authenticates with a long-lived refresh token (tools/dropbox_auth.py) and
// keeps the short-lived access token it trades it for. Calls block for about a
// second each (TLS over Wi-Fi), so call them when the writer has paused.
#pragma once

#include <Arduino.h>
#include <string>
#include <vector>

namespace tw {

class Dropbox {
  public:
    struct Entry {
        std::string name;
        std::string path; // as Dropbox displays it
        std::string rev;
        uint32_t modified = 0; // server_modified, seconds since epoch
        bool folder = false;
    };

    enum class Result {
        Ok,
        NotFound, // the path doesn't exist
        Conflict, // upload: the file changed since the given rev
        Error,    // network or API error; see lastError()
    };

    Dropbox(const char *appKey, const char *refreshToken);

    // Metadata for one file: rev and modified time.
    Result metadata(const std::string &path, Entry &out);

    // Downloads a whole file and its rev.
    Result download(const std::string &path, std::string &data, std::string &rev);

    // Uploads a whole file. With a rev, replaces the file only if it is still that
    // revision (Conflict otherwise); with an empty rev, creates it (Conflict if it
    // already exists). newRev receives the uploaded revision.
    Result upload(const std::string &path, const std::string &data, const std::string &rev, std::string &newRev);

    // Uploads next to path with Dropbox picking a free name, for conflicted copies.
    Result uploadCopy(const std::string &path, const std::string &data, std::string &savedPath);

    // Files directly inside a folder.
    Result list(const std::string &folder, std::vector<Entry> &out);

    const std::string &lastError() const { return lastError_; }

  private:
    bool ensureToken();
    Result call(const char *host, const char *endpoint, const std::string &arg, const std::string *body,
                std::string &response, std::string *apiResult);

    std::string appKey_;
    std::string refreshToken_;
    std::string accessToken_;
    uint32_t tokenExpiresAt_ = 0; // millis()
    std::string lastError_;
};

} // namespace tw
