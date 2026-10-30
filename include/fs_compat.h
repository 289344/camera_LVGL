#pragma once

#include <cstdint>
#include <string>

namespace fscompat {

bool exists(const std::string &path);
bool isDirectory(const std::string &path);
bool createDirectories(const std::string &path);
std::string baseName(const std::string &path);
// 将存在的本地文件路径解析为绝对 file URI；失败返回空串并保留 errno。
std::string fileUri(const std::string &path);
uint64_t fileSize(const std::string &path);
std::string lastWriteTime(const std::string &path);

}  // namespace fscompat
