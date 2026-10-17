#include "file_manager/file_manager.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>

#include "fs_compat.h"

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

std::vector<FileInfo> FileManager::list(const std::string &dir) {
    std::vector<FileInfo> result;
    if (!fscompat::isDirectory(dir)) return result;

    DIR *d = ::opendir(dir.c_str());
    if (!d) return result;

    while (struct dirent *entry = ::readdir(d)) {
        const char *name = entry->d_name;
        if (std::strcmp(name, ".") == 0 || std::strcmp(name, "..") == 0) continue;

        size_t len = std::strlen(name);
        if (len < 4 || std::strcmp(name + len - 4, ".avi") != 0) continue;

        std::string full = dir + "/" + name;
        struct stat st;
        if (::stat(full.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) continue;

        FileInfo info;
        info.name = name;
        info.path = full;
        info.size = static_cast<uint64_t>(st.st_size);
        info.created = fscompat::lastWriteTime(full);
        result.push_back(info);
    }
    ::closedir(d);

    std::sort(result.begin(), result.end(), [](const FileInfo &a, const FileInfo &b) {
        return a.created > b.created;
    });
    return result;
}

}  // namespace filemanager
