#include "file_manager/file_manager.h"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <cstdio>
#include <filesystem>
#include <system_error>

namespace fs = std::filesystem;

namespace filemanager {

std::string FileManager::formatSize(uint64_t size) {
    char buf[32];
    if (size < 1024) {
        std::snprintf(buf, sizeof(buf), "%llu B", static_cast<unsigned long long>(size));
    } else if (size < 1024 * 1024) {
        std::snprintf(buf, sizeof(buf), "%.1f KB", size / 1024.0);
    } else if (size < 1024ULL * 1024 * 1024) {
        std::snprintf(buf, sizeof(buf), "%.1f MB", size / (1024.0 * 1024.0));
    } else {
        std::snprintf(buf, sizeof(buf), "%.2f GB", size / (1024.0 * 1024.0 * 1024.0));
    }
    return std::string(buf);
}

std::string FileManager::formatTime(const std::string &isoTime) {
    // 输入形如 2026-10-16 17:34:55，直接返回，仅作占位
    return isoTime;
}

static std::string lastWriteTime(const fs::path &p) {
    std::error_code ec;
    auto ftime = fs::last_write_time(p, ec);
    if (ec) return "";
    auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
        ftime - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
    std::time_t t = std::chrono::system_clock::to_time_t(sctp);
    std::tm tm{};
    localtime_r(&t, &tm);
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                  tm.tm_hour, tm.tm_min, tm.tm_sec);
    return std::string(buf);
}

std::vector<FileInfo> FileManager::list(const std::string &dir) {
    std::vector<FileInfo> result;
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return result;

    for (const auto &entry : fs::directory_iterator(dir, ec)) {
        if (ec) break;
        if (!entry.is_regular_file(ec)) continue;
        if (entry.path().extension() != ".avi") continue;
        FileInfo info;
        info.name = entry.path().filename().string();
        info.path = entry.path().string();
        info.size = entry.file_size(ec);
        info.created = lastWriteTime(entry.path());
        result.push_back(std::move(info));
    }

    std::sort(result.begin(), result.end(), [](const FileInfo &a, const FileInfo &b) {
        return a.created > b.created;
    });
    return result;
}

}  // namespace filemanager
