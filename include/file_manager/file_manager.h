#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace filemanager {

struct FileInfo {
    std::string name;
    std::string path;
    uint64_t size = 0;
    std::string created;  // 格式化后的时间字符串
};

class FileManager {
public:
    /** 扫描目录下扩展名为 .mp4/.avi 的文件，按创建时间倒序返回。 */
    static std::vector<FileInfo> list(const std::string &dir);

    static std::string formatSize(uint64_t size);
    static std::string formatTime(const std::string &isoTime);
};

}  // namespace filemanager
