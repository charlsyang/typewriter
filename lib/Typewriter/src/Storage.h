// Files in the ESP32's flash (LittleFS), written so a power cut never leaves a
// half-written file behind.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace tw {

namespace storage {

// Mounts the file system, formatting it the first time. Returns false on failure.
bool begin();

bool exists(const char *path);

// Reads a whole file into out. Returns false if it can't be read.
bool read(const char *path, std::string &out);

// Writes a whole file: to "<path>.tmp" first, then renamed over the old file,
// which LittleFS does atomically. Creates parent folders as needed.
bool write(const char *path, const std::string &data);

// Deletes a file. Returns false if it couldn't be removed.
bool remove(const char *path);

// Names of the files directly inside a folder (no subfolders).
std::vector<std::string> list(const char *dir);

// Size of a file in bytes, or -1 if it doesn't exist.
long size(const char *path);

// When a file was last written (Unix time), or 0 if unknown.
uint32_t modified(const char *path);

// Bytes used and total, for the status check.
size_t usedBytes();
size_t totalBytes();

} // namespace storage

} // namespace tw
