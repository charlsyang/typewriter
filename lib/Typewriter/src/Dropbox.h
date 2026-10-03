// A small Dropbox API v2 client: the handful of calls the typewriter needs.
//
// Authenticates with a long-lived refresh token (tools/dropbox_auth.py) and
// keeps the short-lived access token it trades it for. Connections stay open
// between calls; still, calls block, so make them from a background task.
#pragma once

#include <Arduino.h>
#include <NetworkClientSecure.h>
#include <string>
#include <vector>

class HTTPClient;

namespace tw {

class Dropbox {
  public:
    struct Entry {
        std::string name;
        std::string path; // as Dropbox displays it
        std::string id;   // "id:...", kept through renames and moves
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

    // Metadata for one file: rev and modified time. path may be a file ID ("id:...").
    Result metadata(const std::string &path, Entry &out);

    // Downloads a whole file and its rev (and file ID, if id is given).
    Result download(const std::string &path, std::string &data, std::string &rev, std::string *id = nullptr);

    // Downloads only the first maxBytes of a file (e.g. to read front matter).
    Result downloadStart(const std::string &path, size_t maxBytes, std::string &data);

    // Deletes a file (to Dropbox's Deleted files), only if it is still at rev.
    Result remove(const std::string &path, const std::string &rev);

    // Uploads a whole file. With a rev, replaces the file only if it is still that
    // revision (Conflict otherwise); with an empty rev, creates it (Conflict if it
    // already exists). newRev receives the uploaded revision (newId the file ID).
    Result upload(const std::string &path, const std::string &data, const std::string &rev, std::string &newRev,
                  std::string *newId = nullptr);

    // Uploads next to path with Dropbox picking a free name, for conflicted copies.
    Result uploadCopy(const std::string &path, const std::string &data, std::string &savedPath);

    // Files directly inside a folder.
    Result list(const std::string &folder, std::vector<Entry> &out);

    const std::string &lastError() const { return lastError_; }

  private:
    NetworkClientSecure &connection(bool content);
    HTTPClient &httpClient(bool content);
    static std::string tlsError(NetworkClientSecure &tls);
    bool ensureToken();
    Result call(const char *host, const char *endpoint, const std::string &arg, const std::string *body,
                std::string &response, std::string *apiResult, const char *range = nullptr);

    std::string appKey_;
    std::string refreshToken_;
    std::string accessToken_;
    uint32_t tokenExpiresAt_ = 0; // millis()
    std::string lastError_;
    NetworkClientSecure *apiTls_ = nullptr;     // api.dropboxapi.com
    NetworkClientSecure *contentTls_ = nullptr; // content.dropboxapi.com
    HTTPClient *apiHttp_ = nullptr;             // kept too: see httpClient()
    HTTPClient *contentHttp_ = nullptr;
};

} // namespace tw
