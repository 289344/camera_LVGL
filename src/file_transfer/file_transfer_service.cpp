#include "file_transfer/file_transfer_service.h"

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <system_error>

namespace transfer {

FileTransferService::~FileTransferService() {
    cancel();
    if (thread_.joinable()) thread_.join();
}

bool FileTransferService::start(const std::string &srcPath, const std::string &dstDir) {
    if (transferring_.load()) return false;

    std::error_code ec;
    std::filesystem::create_directories(dstDir, ec);
    if (ec) {
        std::lock_guard<std::mutex> lk(resMutex_);
        result_ = {false, "无法创建目标目录: " + dstDir, ""};
        finished_.store(true);
        return false;
    }

    std::string dstPath = dstDir + "/" + std::filesystem::path(srcPath).filename().string();

    {
        std::lock_guard<std::mutex> lk(resMutex_);
        result_ = {};
    }
    cancel_.store(false);
    finished_.store(false);
    copied_.store(0);
    total_.store(0);
    transferring_.store(true);
    thread_ = std::thread(&FileTransferService::worker, this, srcPath, dstPath);
    return true;
}

void FileTransferService::cancel() {
    cancel_.store(true);
}

int FileTransferService::progressPercent() const {
    int64_t total = total_.load();
    if (total <= 0) return 0;
    int64_t copied = copied_.load();
    int pct = static_cast<int>(copied * 100 / total);
    return pct < 0 ? 0 : (pct > 100 ? 100 : pct);
}

FileTransferService::Result FileTransferService::takeResult() {
    std::lock_guard<std::mutex> lk(resMutex_);
    Result r = result_;
    result_ = {};
    return r;
}

void FileTransferService::worker(const std::string &srcPath, const std::string &dstPath) {
    Result res;

    std::ifstream in(srcPath, std::ios::binary);
    if (!in) {
        res = {false, "无法打开源文件: " + std::string(std::strerror(errno)), ""};
    } else {
        std::ofstream out(dstPath, std::ios::binary | std::ios::trunc);
        if (!out) {
            res = {false, "无法创建目标文件: " + std::string(std::strerror(errno)), ""};
        } else {
            in.seekg(0, std::ios::end);
            std::streamoff total = in.tellg();
            in.seekg(0, std::ios::beg);
            total_.store(total > 0 ? total : 0);

            char buf[1024 * 1024];
            while (in && !cancel_.load()) {
                in.read(buf, sizeof(buf));
                std::streamsize n = in.gcount();
                if (n <= 0) break;
                out.write(buf, n);
                copied_.fetch_add(n);
            }
            out.flush();
            if (!out) {
                res = {false, "写入目标文件失败", ""};
            } else if (cancel_.load()) {
                res = {false, "传输已取消", ""};
            } else {
                res = {true, "", dstPath};
            }
        }
    }

    {
        std::lock_guard<std::mutex> lk(resMutex_);
        result_ = res;
    }
    transferring_.store(false);
    finished_.store(true);
}

}  // namespace transfer
