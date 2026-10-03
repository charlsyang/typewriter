// In-memory stand-in for lib/Typewriter/src/Dropbox.h, for host tests of Sync.cpp.
#pragma once

#include <map>
#include <string>
#include <vector>

namespace tw {

class Dropbox {
  public:
    struct Entry {
        std::string name, path, id, rev;
        unsigned modified = 0;
        bool folder = false;
    };
    enum class Result { Ok, NotFound, Conflict, Error };

    std::map<std::string, std::string> files, revs, ids;
    int nextRev = 1, nextId = 1;
    int metadataCalls = 0;
    // Simulates a laptop edit landing between metadata() and upload().
    std::string editBeforeNextUpload;
    // If Dropbox turned an update of a missing path into a new file.
    bool updateCreatesMissing = false;

    Result metadata(const std::string &pathOrId, Entry &out) {
        metadataCalls++;
        std::string path = pathOrId;
        if (pathOrId.rfind("id:", 0) == 0) {
            path.clear();
            for (auto &kv : ids)
                if (kv.second == pathOrId && files.count(kv.first))
                    path = kv.first;
        }
        if (!files.count(path))
            return Result::NotFound;
        out.path = path;
        out.id = ids[path];
        out.rev = revs[path];
        return Result::Ok;
    }
    Result download(const std::string &path, std::string &data, std::string &rev, std::string *id = nullptr) {
        if (!files.count(path))
            return Result::NotFound;
        data = files[path];
        rev = revs[path];
        if (id)
            *id = ids[path];
        return Result::Ok;
    }
    Result upload(const std::string &path, const std::string &data, const std::string &rev, std::string &newRev,
                  std::string *newId = nullptr) {
        if (!editBeforeNextUpload.empty()) {
            put(path, editBeforeNextUpload);
            editBeforeNextUpload.clear();
        }
        bool create = updateCreatesMissing && !rev.empty() && !files.count(path);
        if (!create && (rev.empty() ? files.count(path) != 0 : (!files.count(path) || revs[path] != rev)))
            return Result::Conflict;
        newRev = put(path, data);
        if (newId)
            *newId = ids[path];
        return Result::Ok;
    }
    Result remove(const std::string &path, const std::string &rev) {
        if (!files.count(path))
            return Result::NotFound;
        if (revs[path] != rev)
            return Result::Conflict;
        files.erase(path);
        ids.erase(path);
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
        if (!ids.count(path))
            ids[path] = "id:f" + std::to_string(nextId++);
        return revs[path] = "r" + std::to_string(nextRev++);
    }
    // Test helper: a rename or move made elsewhere. The file keeps its ID and rev.
    void move(const std::string &from, const std::string &to) {
        files[to] = files[from];
        revs[to] = revs[from];
        ids[to] = ids[from];
        files.erase(from);
        revs.erase(from);
        ids.erase(from);
    }

  private:
    std::string error = "fake";
};

} // namespace tw
