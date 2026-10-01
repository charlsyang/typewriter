// In-memory stand-in for lib/Typewriter/src/Storage.h, for host tests.
#pragma once

#include <map>
#include <string>

namespace tw {
namespace storage {
inline std::map<std::string, std::string> &files() {
    static std::map<std::string, std::string> f;
    return f;
}
inline bool read(const char *path, std::string &out) {
    if (!files().count(path))
        return false;
    out = files()[path];
    return true;
}
inline bool write(const char *path, const std::string &data) {
    files()[path] = data;
    return true;
}
} // namespace storage
} // namespace tw
