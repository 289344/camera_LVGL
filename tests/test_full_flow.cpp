// 无 GUI 的完整业务流程测试：音视频录像 -> 文件列表 -> 本机传输
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <thread>
#include <linux/videodev2.h>

#include "camera/camera_device.h"
#include "recording/recording_service.h"
#include "file_manager/file_manager.h"
#include "file_transfer/file_transfer_service.h"

int main() {
    const std::string recDir = "/tmp/test_full_flow/recordings";
    const std::string dstDir = "/tmp/test_full_flow/received";
    std::filesystem::create_directories(recDir);
    std::filesystem::create_directories(dstDir);

    // 1. 录像 1 秒
    camera::CameraDevice cam;
    if (!cam.open("/dev/video1")) { std::printf("FAIL open: %s\n", cam.lastError().c_str()); return 1; }
    if (!cam.setFormat(v4l2_fourcc('M', 'J', 'P', 'G'), 640, 480)) {
        std::printf("FAIL setFormat: %s\n", cam.lastError().c_str()); return 1;
    }

    recording::RecordingService rec;
    recording::RecordingService::Config cfg{recDir, 640, 480, 30, 1};
    cfg.audioDevice = "plughw:1,0";
    if (!rec.start(cfg)) { std::printf("FAIL rec start: %s\n", rec.lastError().c_str()); return 1; }

    if (!cam.start([&](const camera::Frame &f) { rec.pushFrame(f.data, f.size); })) {
        std::printf("FAIL cam start: %s\n", cam.lastError().c_str()); return 1;
    }
    std::this_thread::sleep_for(std::chrono::seconds(1));
    cam.stop();
    rec.stop();
    std::printf("rec: frames=%u bytes=%llu file=%s\n", rec.frameCount(),
                (unsigned long long)rec.bytesWritten(), rec.filePath().c_str());
    if (rec.frameCount() == 0) { std::printf("FAIL: no frames\n"); return 1; }

    // 2. 文件列表
    auto files = filemanager::FileManager::list(recDir);
    std::printf("list: %zu files\n", files.size());
    if (files.empty()) { std::printf("FAIL: no files listed\n"); return 1; }
    bool found = false;
    for (const auto &f : files) if (f.path == rec.filePath()) found = true;
    if (!found) { std::printf("FAIL: recorded file not in list\n"); return 1; }

    // 3. 传输
    transfer::FileTransferService tr;
    if (!tr.start(rec.filePath(), dstDir)) { std::printf("FAIL transfer start\n"); return 1; }
    while (tr.isTransferring()) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    auto r = tr.takeResult();
    std::printf("transfer: success=%d dst=%s err=%s\n", r.success, r.dstPath.c_str(), r.error.c_str());
    if (!r.success) { std::printf("FAIL transfer\n"); return 1; }

    std::error_code ec;
    auto srcSize = std::filesystem::file_size(rec.filePath(), ec);
    auto dstSize = std::filesystem::file_size(r.dstPath, ec);
    if (srcSize != dstSize) { std::printf("FAIL size mismatch %llu != %llu\n",
        (unsigned long long)srcSize, (unsigned long long)dstSize); return 1; }

    std::printf("PASS: full flow ok\n");
    return 0;
}
