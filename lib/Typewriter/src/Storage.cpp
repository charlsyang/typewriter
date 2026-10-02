#include "Storage.h"

#include <LittleFS.h>

namespace tw {

namespace storage {

bool begin() {
    return LittleFS.begin(true); // format if mounting fails (first boot)
}

bool exists(const char *path) {
    return LittleFS.exists(path);
}

bool read(const char *path, std::string &out) {
    File f = LittleFS.open(path, "r");
    if (!f)
        return false;
    out.clear();
    out.reserve(f.size());
    char buf[512];
    while (f.available()) {
        size_t n = f.read(reinterpret_cast<uint8_t *>(buf), sizeof(buf));
        if (n == 0)
            break;
        out.append(buf, n);
    }
    f.close();
    return true;
}

// Creates every folder above path, e.g. "/daily" for "/daily/2026-09-29.md".
static void makeParents(const char *path) {
    std::string p(path);
    for (size_t i = 1; i < p.size(); i++) {
        if (p[i] == '/') {
            std::string dir = p.substr(0, i);
            if (!LittleFS.exists(dir.c_str()))
                LittleFS.mkdir(dir.c_str());
        }
    }
}

bool write(const char *path, const std::string &data) {
    makeParents(path);
    std::string tmp = std::string(path) + ".tmp";
    File f = LittleFS.open(tmp.c_str(), "w");
    if (!f)
        return false;
    size_t written = f.write(reinterpret_cast<const uint8_t *>(data.data()), data.size());
    f.close();
    if (written != data.size()) {
        LittleFS.remove(tmp.c_str());
        return false;
    }
    return LittleFS.rename(tmp.c_str(), path);
}

bool remove(const char *path) {
    return LittleFS.remove(path);
}

std::vector<std::string> list(const char *dir) {
    std::vector<std::string> names;
    File d = LittleFS.open(dir);
    if (!d || !d.isDirectory())
        return names;
    for (File f = d.openNextFile(); f; f = d.openNextFile()) {
        if (!f.isDirectory())
            names.push_back(f.name());
    }
    return names;
}

long size(const char *path) {
    File f = LittleFS.open(path, "r");
    if (!f)
        return -1;
    long n = f.size();
    f.close();
    return n;
}

size_t usedBytes() {
    return LittleFS.usedBytes();
}

size_t totalBytes() {
    return LittleFS.totalBytes();
}

} // namespace storage

} // namespace tw
