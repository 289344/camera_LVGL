#include "fs_compat.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <sys/stat.h>
#include <sys/types.h>

namespace fscompat {

bool exists(const std::string &path) {
    struct stat st;
    return ::stat(path.c_str(), &st) == 0;
}

bool isDirectory(const std::string &path) {
    struct stat st;
    if (::stat(path.c_str(), &st) != 0) return false;
    return S_ISDIR(st.st_mode);
}

bool createDirectories(const std::string &path) {
    if (path.empty()) return false;

    std::string cur;
    size_t i = 0;
    if (path[0] == '/') {
        cur = "/";
        i = 1;
    }

    while (i <= path.size()) {
        size_t j = path.find('/', i);
        if (j == std::string::npos) {
            if (i < path.size()) cur += path.substr(i);
            i = path.size() + 1;
        } else {
            cur += path.substr(i, j - i);
            i = j + 1;
        }

        if (cur.empty() || cur == "/") continue;

        struct stat st;
        if (::stat(cur.c_str(), &st) != 0) {
            if (::mkdir(cur.c_str(), 0755) != 0 && errno != EEXIST) {
                return false;
            }
        } else if (!S_ISDIR(st.st_mode)) {
            return false;
        }
    }

    return true;
}

std::string baseName(const std::string &path) {
    size_t pos = path.find_last_of('/');
    if (pos == std::string::npos) return path;
    return path.substr(pos + 1);
}

uint64_t fileSize(const std::string &path) {
    struct stat st;
    if (::stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) return 0;
    return static_cast<uint64_t>(st.st_size);
}

std::string lastWriteTime(const std::string &path) {
    struct stat st;
    if (::stat(path.c_str(), &st) != 0) return "";

    std::time_t t = st.st_mtime;
    std::tm tm;
    localtime_r(&t, &tm);
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                  tm.tm_hour, tm.tm_min, tm.tm_sec);
    return std::string(buf);
}

}  // namespace fscompat
