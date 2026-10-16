#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

namespace transfer {

/** 本机文件复制服务：在后台线程执行，支持进度查询与取消。 */
class FileTransferService {
public:
    struct Result {
        bool success = false;
        std::string error;
        std::string dstPath;
    };

    FileTransferService() = default;
    ~FileTransferService();

    FileTransferService(const FileTransferService &) = delete;
    FileTransferService &operator=(const FileTransferService &) = delete;

    bool start(const std::string &srcPath, const std::string &dstDir);
    void cancel();

    bool isTransferring() const { return transferring_.load(); }
    int progressPercent() const;
    bool finished() const { return finished_.load(); }
    Result takeResult();  // 取走并清除上次结果

private:
    void worker(const std::string &srcPath, const std::string &dstPath);

    std::thread thread_;
    std::atomic<bool> transferring_{false};
    std::atomic<bool> cancel_{false};
    std::atomic<bool> finished_{false};
    std::atomic<int64_t> copied_{0};
    std::atomic<int64_t> total_{0};

    std::mutex resMutex_;
    Result result_;
};

}  // namespace transfer
