#include "app/app_controller.h"

#include <algorithm>
#include <climits>
#include <linux/videodev2.h>

#include "ui/ui.h"
#include "preview/pixel_converter.h"
#include "file_manager/file_manager.h"
#include "file_transfer/file_transfer_service.h"

namespace {
constexpr uint32_t kMJPG = v4l2_fourcc('M', 'J', 'P', 'G');
constexpr uint32_t kYUYV = v4l2_fourcc('Y', 'U', 'Y', 'V');
}  // namespace

AppController::AppController() {
    // 默认目录：项目根目录下的 recordings / received
    recordingsDir_ = "recordings";
    receivedDir_ = "received";
}

AppController::~AppController() {
    stopPreview();
}

void AppController::init(Ui *ui) {
    ui_ = ui;
    buildCameraList();
    ui_->setCameraList(cameras_, selectedCamera_);
    ui_->setSaveDirText(recordingsDir_);
    refreshFileList();
}

void AppController::buildCameraList() {
    cameras_.clear();
    for (const char *p : {"/dev/video0", "/dev/video1"}) {
        camera::CameraDevice probe;
        if (probe.open(p)) {
            cameras_.emplace_back(p);
            probe.close();
        }
    }
    if (cameras_.empty()) cameras_.emplace_back("(无摄像头)");
}

void AppController::selectCamera(int index) {
    if (index < 0 || index >= static_cast<int>(cameras_.size())) return;
    bool wasPreviewing = previewing_.load();
    if (wasPreviewing) stopPreview();
    selectedCamera_ = index;
    if (wasPreviewing) startPreview();
}

bool AppController::chooseFormat(camera::CameraDevice &dev, uint32_t &pixfmt, uint32_t &w, uint32_t &h) {
    auto fmts = dev.listFormats();
    if (fmts.empty()) return false;

    auto score = [](const camera::VideoFormat &f) -> int {
        // 越接近 640x480 得分越高
        long long dw = static_cast<long long>(f.width) - 640;
        long long dh = static_cast<long long>(f.height) - 480;
        return static_cast<int>(dw * dw + dh * dh);
    };

    // 优先 MJPG，其次 YUYV
    for (uint32_t want : {kMJPG, kYUYV}) {
        camera::VideoFormat best;
        bool found = false;
        int bestScore = INT_MAX;
        for (const auto &f : fmts) {
            if (f.pixelformat != want) continue;
            int s = score(f);
            if (!found || s < bestScore) {
                best = f;
                bestScore = s;
                found = true;
            }
        }
        if (found) {
            pixfmt = best.pixelformat;
            w = best.width;
            h = best.height;
            return true;
        }
    }
    // 兜底：第一个格式
    pixfmt = fmts[0].pixelformat;
    w = fmts[0].width;
    h = fmts[0].height;
    return true;
}

void AppController::startPreview() {
    if (!ui_) return;
    if (previewing_.load()) return;
    if (selectedCamera_ < 0 || selectedCamera_ >= static_cast<int>(cameras_.size())) {
        ui_->setStatusText("无可用摄像头", 0xff5555);
        return;
    }

    const std::string path = cameras_[selectedCamera_];
    if (!camera_.open(path)) {
        ui_->setStatusText("打开摄像头失败: " + path, 0xff5555);
        return;
    }

    uint32_t pixfmt = 0, w = 0, h = 0;
    if (!chooseFormat(camera_, pixfmt, w, h) || !camera_.setFormat(pixfmt, w, h)) {
        std::string err = camera_.lastError();
        camera_.close();
        ui_->setStatusText("格式错误: " + err, 0xff5555);
        return;
    }

    previewW_ = w;
    previewH_ = h;
    pool_ = std::make_unique<preview::FramePool>(w, h, 3);
    displayedFrame_ = -1;

    if (!camera_.start([this](const camera::Frame &f) { handleFrame(f); })) {
        std::string err = camera_.lastError();
        camera_.close();
        pool_.reset();
        ui_->setStatusText("采集失败: " + err, 0xff5555);
        return;
    }

    previewing_.store(true);
    ui_->setPreviewInfoText("启动中...");
    ui_->setStatusText("预览中: " + path, 0x66cc66);
}

void AppController::stopPreview() {
    if (recording_.load()) stopRecording();
    if (previewing_.load() || camera_.isOpen()) {
        camera_.stop();
        camera_.close();
    }
    if (pool_) {
        if (displayedFrame_ >= 0) pool_->release(displayedFrame_);
        displayedFrame_ = -1;
        pool_.reset();
    }
    previewing_.store(false);
    if (ui_) {
        ui_->clearPreview();
        ui_->setStatusText("预览已停止");
    }
}

void AppController::handleFrame(const camera::Frame &frame) {
    if (!pool_) return;
    int idx = pool_->acquireWrite();
    if (idx < 0) return;  // 没有可用缓冲，丢弃本帧
    uint16_t *dst = reinterpret_cast<uint16_t *>(pool_->data(idx));

    bool ok = false;
    if (frame.pixelformat == kMJPG) {
        uint32_t ow = 0, oh = 0;
        ok = preview::jpegToRgb565(frame.data, frame.size, dst, pool_->width(), pool_->height(), ow, oh);
    } else if (frame.pixelformat == kYUYV) {
        preview::yuyvToRgb565(frame.data, dst, frame.width, frame.height);
        ok = true;
    }

    if (ok) {
        pool_->publish(idx);
        if (recording_.load()) {
            if (frame.pixelformat == kMJPG) {
                recorder_.pushFrame(frame.data, frame.size);
            } else if (frame.pixelformat == kYUYV) {
                if (preview::yuyvToJpeg(frame.data, frame.width, frame.height, jpegEncodeBuf_)) {
                    recorder_.pushFrame(jpegEncodeBuf_.data(), jpegEncodeBuf_.size());
                }
            }
        }
    } else {
        pool_->release(idx);
        std::lock_guard<std::mutex> lk(errMutex_);
        if (pendingError_.empty()) pendingError_ = "帧解码失败";
    }
}

std::string AppController::popError() {
    std::lock_guard<std::mutex> lk(errMutex_);
    std::string e = pendingError_;
    pendingError_.clear();
    return e;
}

void AppController::tick() {
    if (!ui_) return;

    if (++fileRefreshCounter_ % 30 == 0) refreshFileList();

    std::string err = popError();
    if (!err.empty()) ui_->setStatusText(err, 0xff5555);

    if (recording_.load()) {
        uint32_t secs = static_cast<uint32_t>(recorder_.durationSec());
        ui_->setRecordState(true, secs);
    }

    if (transfer_.isTransferring() || transfer_.finished()) {
        ui_->setTransferProgress(transfer_.progressPercent());
    }
    if (transfer_.finished()) {
        auto r = transfer_.takeResult();
        if (r.success) {
            ui_->setTransferProgress(100);
            ui_->setTransferStatus("成功 -> " + r.dstPath, 0x66cc66);
            ui_->setStatusText("传输完成", 0x66cc66);
        } else {
            ui_->setTransferStatus("失败: " + r.error, 0xff5555);
            ui_->setStatusText("传输失败", 0xff5555);
        }
    }

    if (previewing_.load() && pool_) {
        int idx = pool_->tryGetLatest();
        if (idx >= 0) {
            ui_->setPreviewFrame(pool_->data(idx), pool_->width(), pool_->height());
            ui_->setPreviewInfoText("");
            if (displayedFrame_ >= 0) pool_->release(displayedFrame_);
            displayedFrame_ = idx;
        }
    }
}

void AppController::startRecording() {
    if (!ui_) return;
    if (recording_.load()) return;
    if (!previewing_.load() || !pool_) {
        ui_->setStatusText("请先开始预览", 0xffaa44);
        return;
    }

    recording::RecordingService::Config cfg;
    cfg.dir = recordingsDir_;
    cfg.width = previewW_;
    cfg.height = previewH_;
    cfg.fpsNum = 30;
    cfg.fpsDen = 1;

    if (!recorder_.start(cfg)) {
        ui_->setStatusText("录像失败: " + recorder_.lastError(), 0xff5555);
        return;
    }
    recording_.store(true);
    ui_->setRecordButtons(true);
    ui_->setRecordState(true, 0);
    ui_->setStatusText("录像已开始", 0x66cc66);
}

void AppController::stopRecording() {
    if (!recording_.load()) return;
    recorder_.stop();
    recording_.store(false);
    if (ui_) {
        ui_->setRecordButtons(false);
        ui_->setRecordState(false, 0);
        if (recorder_.lastError().empty()) {
            ui_->setStatusText("已保存: " + recorder_.filePath(), 0x66cc66);
        } else {
            ui_->setStatusText("录像错误: " + recorder_.lastError(), 0xff5555);
        }
    }
    refreshFileList();
}

void AppController::toggleRecording() {
    if (recording_.load()) stopRecording();
    else startRecording();
}

void AppController::refreshFileList() {
    files_ = filemanager::FileManager::list(recordingsDir_);
    if (!ui_) return;
    std::vector<std::vector<std::string>> rows;
    rows.reserve(files_.size());
    for (const auto &f : files_) {
        rows.push_back({f.name, filemanager::FileManager::formatSize(f.size), f.created});
    }
    ui_->setFileTable(rows);
    if (selectedFile_ >= static_cast<int>(files_.size())) {
        selectedFile_ = -1;
        ui_->setTransferEnabled(false);
    }
}

void AppController::selectFile(int row) {
    selectedFile_ = row;
    if (ui_) {
        ui_->setTransferEnabled(row >= 0 && row < static_cast<int>(files_.size()));
    }
}

void AppController::transferSelectedFile() {
    if (!ui_) return;
    if (transfer_.isTransferring()) {
        ui_->setStatusText("已有传输进行中", 0xffaa44);
        return;
    }
    if (selectedFile_ < 0 || selectedFile_ >= static_cast<int>(files_.size())) {
        ui_->setStatusText("请先选择文件", 0xffaa44);
        return;
    }

    const std::string &src = files_[selectedFile_].path;
    if (!transfer_.start(src, receivedDir_)) {
        auto r = transfer_.takeResult();
        ui_->setTransferStatus("失败: " + r.error, 0xff5555);
        ui_->setStatusText("传输失败", 0xff5555);
        return;
    }

    ui_->setTransferProgress(0);
    ui_->setTransferStatus("传输中...", 0xcccccc);
    ui_->setStatusText("传输已开始", 0x66cc66);
}
