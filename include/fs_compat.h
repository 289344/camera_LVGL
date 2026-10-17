#pragma once

#include <cstdint>
#include <string>

namespace fscompat {

bool exists(const std::string &path);
bool isDirectory(const std::string &path);
bool createDirectories(const std::string &path);
std::string baseName(const std::string &path);
uint64_t fileSize(const std::string &path);
std::string lastWriteTime(const std::string &path);

}  // namespace fscompat
