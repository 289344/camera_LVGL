#include "file_transfer/file_transfer_service.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include "fs_compat.h"

namespace transfer {

namespace {

const char kTcpMagic[4] = {'C', 'A', 'M', 'X'};

void putU32(uint8_t *dst, uint32_t v) {
    dst[0] = static_cast<uint8_t>((v >> 24) & 0xff);
    dst[1] = static_cast<uint8_t>((v >> 16) & 0xff);
    dst[2] = static_cast<uint8_t>((v >> 8) & 0xff);
    dst[3] = static_cast<uint8_t>(v & 0xff);
}

void putU64(uint8_t *dst, uint64_t v) {
    for (int i = 0; i < 8; ++i) {
        dst[i] = static_cast<uint8_t>((v >> (56 - i * 8)) & 0xff);
    }
}

bool sendAll(int fd, const uint8_t *data, size_t size) {
    size_t off = 0;
    while (off < size) {
        ssize_t n = ::send(fd, data + off, size - off, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (n == 0) return false;
        off += static_cast<size_t>(n);
    }
    return true;
}

std::string portString(uint16_t port) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%u", static_cast<unsigned>(port));
    return std::string(buf);
}

}  // namespace

FileTransferService::~FileTransferService() {
    cancel();
    if (thread_.joinable()) thread_.join();
}

bool FileTransferService::start(const std::string &srcPath, const std::string &dstDir) {
    if (transferring_.load()) return false;

    if (!fscompat::createDirectories(dstDir)) {
        std::lock_guard<std::mutex> lk(resMutex_);
        result_ = Result(false, "无法创建目标目录: " + dstDir, "");
        finished_.store(true);
        return false;
    }

    std::string dstPath = dstDir + "/" + fscompat::baseName(srcPath);

    {
        std::lock_guard<std::mutex> lk(resMutex_);
        result_ = Result();
    }
    cancel_.store(false);
    finished_.store(false);
    copied_.store(0);
    total_.store(0);
    transferring_.store(true);
    thread_ = std::thread(&FileTransferService::worker, this, srcPath, dstPath);
    return true;
}

bool FileTransferService::startTcp(const std::string &srcPath, const std::string &host, uint16_t port) {
    if (transferring_.load()) return false;

    {
        std::lock_guard<std::mutex> lk(resMutex_);
        result_ = Result();
    }
    cancel_.store(false);
    finished_.store(false);
    copied_.store(0);
    total_.store(0);
    transferring_.store(true);
    thread_ = std::thread(&FileTransferService::tcpWorker, this, srcPath, host, port);
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
    result_ = Result();
    finished_.store(false);  // 清除完成标志，避免重复取到空结果
    return r;
}

void FileTransferService::worker(const std::string &srcPath, const std::string &dstPath) {
    Result res;

    std::ifstream in(srcPath, std::ios::binary);
    if (!in) {
        res = Result(false, "无法打开源文件: " + std::string(std::strerror(errno)), "");
    } else {
        std::ofstream out(dstPath, std::ios::binary | std::ios::trunc);
        if (!out) {
            res = Result(false, "无法创建目标文件: " + std::string(std::strerror(errno)), "");
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
                res = Result(false, "写入目标文件失败", "");
            } else if (cancel_.load()) {
                res = Result(false, "传输已取消", "");
            } else {
                res = Result(true, "", dstPath);
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

void FileTransferService::tcpWorker(const std::string &srcPath, const std::string &host, uint16_t port) {
    Result res;
    int fd = -1;

    std::string base = fscompat::baseName(srcPath);
    uint64_t total = fscompat::fileSize(srcPath);

    std::ifstream in(srcPath, std::ios::binary);
    if (!in) {
        res = Result(false, "无法打开源文件: " + std::string(std::strerror(errno)), "");
    } else if (total == 0) {
        res = Result(false, "源文件为空", "");
    } else {
        struct sockaddr_in addr;
        std::memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
            res = Result(false, "无效的接收地址: " + host, "");
        } else {
            fd = ::socket(AF_INET, SOCK_STREAM, 0);
            if (fd < 0) {
                res = Result(false, "创建 socket 失败: " + std::string(std::strerror(errno)), "");
            } else if (::connect(fd, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) != 0) {
                res = Result(false, "连接接收端失败: " + std::string(std::strerror(errno)), "");
                ::close(fd);
                fd = -1;
            } else {
                total_.store(static_cast<int64_t>(total));

                uint8_t header[16];
                std::memcpy(header, kTcpMagic, 4);
                putU32(header + 4, static_cast<uint32_t>(base.size()));
                putU64(header + 8, total);

                bool ok = sendAll(fd, header, sizeof(header));
                if (ok) {
                    ok = sendAll(fd, reinterpret_cast<const uint8_t *>(base.data()), base.size());
                }

                char buf[1024 * 1024];
                while (ok && in && !cancel_.load()) {
                    in.read(buf, sizeof(buf));
                    std::streamsize n = in.gcount();
                    if (n <= 0) break;
                    ok = sendAll(fd, reinterpret_cast<const uint8_t *>(buf), static_cast<size_t>(n));
                    if (ok) copied_.fetch_add(static_cast<int64_t>(n));
                }

                if (!ok) {
                    res = Result(false, "发送数据失败", "");
                } else if (cancel_.load()) {
                    res = Result(false, "传输已取消", "");
                } else {
                    ::shutdown(fd, SHUT_WR);
                    char ack = 0;
                    ssize_t n = ::recv(fd, &ack, 1, 0);
                    if (n == 1 && ack == '1') {
                        res = Result(true, "", host + ":" + portString(port) + "/" + base);
                    } else if (n == 1 && ack == '0') {
                        res = Result(false, "接收端写入文件失败", "");
                    } else {
                        res = Result(false, "未收到接收端确认", "");
                    }
                }
            }
        }
    }

    if (fd >= 0) ::close(fd);

    {
        std::lock_guard<std::mutex> lk(resMutex_);
        result_ = res;
    }
    transferring_.store(false);
    finished_.store(true);
}

}  // namespace transfer
