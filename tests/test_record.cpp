// 简易端到端录像测试：采集 MJPG 帧并封装为 AVI（无需 GUI）
#include <chrono>
#include <cstdio>
#include <thread>
#include <linux/videodev2.h>

#include "camera/camera_device.h"
#include "recording/recording_service.h"

int main() {
    camera::CameraDevice cam;
    if (!cam.open("/dev/video0")) {
        std::printf("open failed: %s\n", cam.lastError().c_str());
        return 1;
    }
    if (!cam.setFormat(v4l2_fourcc('M', 'J', 'P', 'G'), 640, 480)) {
        std::printf("setFormat failed: %s\n", cam.lastError().c_str());
        return 1;
    }

    recording::RecordingService rec;
    recording::RecordingService::Config cfg;
    cfg.dir = "/tmp/test_recordings";
    cfg.width = 640;
    cfg.height = 480;
    cfg.fpsNum = 30;
    cfg.fpsDen = 1;
    if (!rec.start(cfg)) {
        std::printf("rec start failed: %s\n", rec.lastError().c_str());
        return 1;
    }

    bool ok = cam.start([&](const camera::Frame &f) {
        rec.pushFrame(f.data, f.size);
    });
    if (!ok) {
        std::printf("cam start failed: %s\n", cam.lastError().c_str());
        return 1;
    }

    std::this_thread::sleep_for(std::chrono::seconds(2));

    cam.stop();
    rec.stop();

    std::printf("file=%s frames=%u bytes=%llu duration=%.2fs dropped=%u err=%s\n",
                rec.filePath().c_str(), rec.frameCount(),
                (unsigned long long)rec.bytesWritten(), rec.durationSec(),
                rec.droppedFrames(), rec.lastError().c_str());
    return 0;
}
