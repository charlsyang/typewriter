// In-memory stand-in for lib/Typewriter/src/Dropbox.h, for host tests of Sync.cpp.
#pragma once

#include <map>
#include <string>
#include <vector>

namespace tw {

class Dropbox {
  public:
    struct Entry {
        std::string name, path, rev;
        unsigned modified = 0;
        bool folder = false;
    };
    enum class Result { Ok, NotFound, Conflict, Error };

    std::map<std::string, std::string> files, revs;
    int nextRev = 1;
    // Simulates a laptop edit landing between metadata() and upload().
    std::string editBeforeNextUpload;

    Result metadata(const std::string &path, Entry &out) {
        if (!files.count(path))
            return Result::NotFound;
        out.rev = revs[path];
        return Result::Ok;
    }
    Result download(const std::string &path, std::string &data, std::string &rev) {
        if (!files.count(path))
            return Result::NotFound;
        data = files[path];
        rev = revs[path];
        return Result::Ok;
    }
    Result upload(const std::string &path, const std::string &data, const std::string &rev, std::string &newRev) {
        if (!editBeforeNextUpload.empty()) {
            put(path, editBeforeNextUpload);
            editBeforeNextUpload.clear();
        }
        if (rev.empty() ? files.count(path) != 0 : (!files.count(path) || revs[path] != rev))
            return Result::Conflict;
        newRev = put(path, data);
        return Result::Ok;
    }
    Result uploadCopy(const std::string &path, const std::string &data, std::string &savedPath) {
        savedPath = path;
        put(path, data);
        return Result::Ok;
    }
    const std::string &lastError() const { return error; }

    // Test helper: a change made elsewhere (the laptop).
    std::string put(const std::string &path, const std::string &data) {
        files[path] = data;
        return revs[path] = "r" + std::to_string(nextRev++);
    }

  private:
    std::string error = "fake";
};

} // namespace tw
