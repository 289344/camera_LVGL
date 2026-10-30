#include "fs_compat.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
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
    size_t pos = 0;
    if (path[0] == '/') {
        cur = "/";
        pos = 1;
    }

    while (pos < path.size()) {
        while (pos < path.size() && path[pos] == '/') ++pos;
        if (pos >= path.size()) break;

        const size_t slash = path.find('/', pos);
        const size_t end = slash == std::string::npos ? path.size() : slash;
        const std::string component = path.substr(pos, end - pos);

        if (!cur.empty() && cur.back() != '/') cur += '/';
        cur += component;

        struct stat st;
        if (::stat(cur.c_str(), &st) != 0) {
            if (::mkdir(cur.c_str(), 0755) != 0 && errno != EEXIST) {
                return false;
            }
        } else if (!S_ISDIR(st.st_mode)) {
            return false;
        }

        pos = end;
    }

    return true;
}

std::string baseName(const std::string &path) {
    size_t pos = path.find_last_of('/');
    if (pos == std::string::npos) return path;
    return path.substr(pos + 1);
}

std::string fileUri(const std::string &path) {
    char *resolved = ::realpath(path.c_str(), nullptr);
    if (!resolved) return "";
    const std::string absolutePath(resolved);
    std::free(resolved);

    // file:// 后必须是绝对路径；空格、#、% 和 UTF-8 字节需要按 URI 编码，
    // 否则相对目录可能被当作主机名，文件名也可能被截为 fragment/query。
    constexpr char hex[] = "0123456789ABCDEF";
    std::string uri = "file://";
    for (unsigned char c : absolutePath) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '/' || c == '-' ||
            c == '_' || c == '.' || c == '~') {
            uri += static_cast<char>(c);
        } else {
            uri += '%';
            uri += hex[c >> 4];
            uri += hex[c & 0x0f];
        }
    }
    return uri;
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
