// A small Readwise API v2 client: the export endpoint, for keeping the
// highlights on the SD card up to date. Authenticates with the access token
// from readwise.io/access_token. Calls block (each a fresh TLS connection,
// ~3 s, and the reply is held in PSRAM while parsed), so make them from a
// background task.
#pragma once

#include "HighlightStore.h"

#include <functional>
#include <string>

namespace tw {

class Readwise {
  public:
    explicit Readwise(const char *token) : token_(token) {}

    // One page of GET /api/v2/export/: highlights updated after `after` (ISO
    // time, UTC), deleted ones included, each passed to onHighlight. cursor: ""
    // for the first page; afterwards the next page's cursor, or "" when done.
    bool exportPage(const std::string &after, std::string &cursor,
                    const std::function<void(const highlights::Incoming &)> &onHighlight);

    const std::string &lastError() const { return lastError_; }
    size_t lastBytes() const { return lastBytes_; } // size of the last page's reply

  private:
    std::string token_;
    std::string lastError_;
    size_t lastBytes_ = 0;
};

} // namespace tw
