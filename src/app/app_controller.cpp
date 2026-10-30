#include "app/app_controller.h"

#include <algorithm>
#include <cerrno>
#include <climits>
#include <csignal>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <linux/videodev2.h>
#include <sstream>
#include <sys/wait.h>
#include <unistd.h>

#include "ui/ui.h"
#include "preview/pixel_converter.h"
#include "file_manager/file_manager.h"
#include "file_transfer/file_transfer_service.h"
#include "fs_compat.h"

namespace {
constexpr uint32_t kMJPG = v4l2_fourcc('M', 'J', 'P', 'G');
constexpr uint32_t kYUYV = v4l2_fourcc('Y', 'U', 'Y', 'V');
constexpr uint16_t kPreviewPort = 9002;
constexpr uint16_t kAudioPort = 9003;
constexpr uint32_t kLiveAudioSampleRate = 16000;
constexpr uint32_t kLiveAudioChannels = 1;

#ifdef CAMERA_LVGL_BOARD
// 板端传输目标：PC 接收服务
constexpr const char *kDefaultTransferHost = "10.151.36.173";
constexpr uint16_t kTransferPort = 9000;
#endif

std::string loadTransferHost() {
#ifdef CAMERA_LVGL_BOARD
    const char *envHost = std::getenv("CAMERA_SERVER_IP");
    if (envHost && *envHost) return envHost;

    std::ifstream cfg("/root/camera_lvgl/server_ip.txt");
    std::string host;
    if (cfg && std::getline(cfg, host)) {
        const size_t first = host.find_first_not_of(" \t\r\n");
        const size_t last = host.find_last_not_of(" \t\r\n");
        if (first != std::string::npos) {
            return host.substr(first, last - first + 1);
        }
    }
    return kDefaultTransferHost;
#else
    return "127.0.0.1";
#endif
}

std::string loadAudioDevice() {
#ifdef CAMERA_LVGL_BOARD
    const char *envDevice = std::getenv("CAMERA_AUDIO_DEVICE");
    if (envDevice && *envDevice) return envDevice;

    std::ifstream cfg("/root/camera_lvgl/audio_device.txt");
    std::string device;
    if (cfg && std::getline(cfg, device)) {
        const size_t first = device.find_first_not_of(" \t\r\n");
        const size_t last = device.find_last_not_of(" \t\r\n");
        if (first != std::string::npos) {
            return device.substr(first, last - first + 1);
        }
    }

    // 优先选择新增的 USB 麦克风；未插 USB 时退回板载 WM8960。
    std::ifstream cards("/proc/asound/cards");
    std::string line;
    while (cards && std::getline(cards, line)) {
        if (line.find("USB-Audio") == std::string::npos &&
            line.find("UACDemo") == std::string::npos) {
            continue;
        }
        std::istringstream in(line);
        int card = -1;
        if (in >> card && card >= 0) {
            return "plughw:" + std::to_string(card) + ",0";
        }
    }
    return "plughw:0,0";
#else
    return "default";
#endif
}
}  // namespace

AppController::AppController() {
    // 默认目录：项目根目录下的 recordings / received
    recordingsDir_ = "recordings";
    receivedDir_ = "received";
    transferHost_ = loadTransferHost();
    audioDevice_ = loadAudioDevice();
}

AppController::~AppController() {
    stopPreview();
    if (playbackPid_ > 0) {
        ::kill(static_cast<pid_t>(playbackPid_), SIGTERM);
        ::waitpid(static_cast<pid_t>(playbackPid_), nullptr, 0);
        playbackPid_ = -1;
    }
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
    stopNetworkPreview();
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

void AppController::toggleNetworkPreview() {
    if (!ui_) return;
    if (networkPreviewing_.load()) {
        stopNetworkPreview();
        ui_->setStatusText("网络预览已停止");
        return;
    }

    if (!previewing_.load()) startPreview();
    if (!previewing_.load() || !pool_) {
        ui_->setStatusText("网络预览需要先成功启动摄像头", 0xff5555);
        ui_->setNetworkPreviewState(false, false);
        return;
    }

    network_preview::NetworkPreviewService::Config cfg;
    cfg.host = transferHost_;
    cfg.port = kPreviewPort;
    cfg.width = previewW_;
    cfg.height = previewH_;
    cfg.maxFps = 20;
    if (!startMicrophoneIfNeeded()) {
        ui_->setStatusText("网络直播启动失败: " + microphone_.lastError(), 0xff5555);
        ui_->setNetworkPreviewState(false, false);
        return;
    }

    network_preview::NetworkAudioService::Config audioCfg;
    audioCfg.host = transferHost_;
    audioCfg.port = kAudioPort;
    audioCfg.sampleRate = kLiveAudioSampleRate;
    audioCfg.channels = kLiveAudioChannels;
    if (!networkAudio_.start(audioCfg)) {
        stopMicrophoneIfUnused();
        ui_->setStatusText("音频直播启动失败: " + networkAudio_.lastError(), 0xff5555);
        ui_->setNetworkPreviewState(false, false);
        return;
    }

    if (!networkPreview_.start(cfg)) {
        networkAudio_.stop();
        stopMicrophoneIfUnused();
        ui_->setStatusText("视频直播启动失败: " + networkPreview_.lastError(), 0xff5555);
        ui_->setNetworkPreviewState(false, false);
        return;
    }

    networkPreviewing_.store(true);
    networkConnectedNotified_ = false;
    ui_->setNetworkPreviewState(true, false);
    ui_->setStatusText("网络音视频直播已启动，正在连接 PC");
}

void AppController::stopNetworkPreview() {
    const bool wasActive = networkPreviewing_.exchange(false);
    networkConnectedNotified_ = false;
    networkPreview_.stop();
    networkAudio_.stop();
    stopMicrophoneIfUnused();
    if (ui_) ui_->setNetworkPreviewState(false, false);
    if (wasActive && ui_) ui_->setStatusText("网络音视频直播已停止");
}

bool AppController::startMicrophoneIfNeeded() {
    if (microphone_.isActive()) return true;

    audio::MicrophoneCapture::Config cfg;
    cfg.device = audioDevice_;
    cfg.sampleRate = kLiveAudioSampleRate;
    cfg.channels = kLiveAudioChannels;
    cfg.periodFrames = 640;
    cfg.bufferFrames = 1920;
    microphoneErrorNotified_ = false;

    const bool started = microphone_.start(
        cfg, [this](const uint8_t *data, size_t size) {
            if (networkPreviewing_.load()) networkAudio_.pushPcm(data, size);
            if (recording_.load()) recorder_.pushAudio(data, size);
        });
    if (!started) return false;

    microphoneErrorNotified_ = false;
    return true;
}

void AppController::stopMicrophoneIfUnused() {
    if (!networkPreviewing_.load() && !recording_.load()) microphone_.stop();
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
        const bool needJpeg = recording_.load() || networkPreviewing_.load();
        const uint8_t *jpegData = nullptr;
        size_t jpegSize = 0;
        if (needJpeg && frame.pixelformat == kMJPG) {
            jpegData = frame.data;
            jpegSize = frame.size;
        } else if (needJpeg && frame.pixelformat == kYUYV &&
                   preview::yuyvToJpeg(frame.data, frame.width, frame.height, jpegEncodeBuf_)) {
            jpegData = jpegEncodeBuf_.data();
            jpegSize = jpegEncodeBuf_.size();
        }
        if (jpegData && recording_.load()) {
            recorder_.pushFrame(jpegData, jpegSize);
        }
        if (jpegData && networkPreviewing_.load()) {
            networkPreview_.pushFrame(jpegData, jpegSize);
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

    if ((networkPreviewing_.load() || recording_.load()) &&
        !microphone_.isActive() && !microphoneErrorNotified_) {
        microphoneErrorNotified_ = true;
        const std::string micError = microphone_.lastError().empty()
            ? "麦克风采集已停止，请检查 USB 麦克风和 /tmp/camera_microphone.log"
            : microphone_.lastError();
        microphone_.stop();
        if (recording_.load()) stopRecording();
        if (networkPreviewing_.load()) stopNetworkPreview();
        ui_->setStatusText(micError, 0xff5555);
    }

    if (networkPreviewing_.load()) {
        const bool connected = networkPreview_.isConnected() && networkAudio_.isConnected();
        ui_->setNetworkPreviewState(true, connected);
        if (connected && !networkConnectedNotified_) {
            networkConnectedNotified_ = true;
            ui_->setStatusText("网络音视频直播中", 0x66cc66);
        } else if (!connected) {
            networkConnectedNotified_ = false;
        }
    }

    if (recording_.load()) {
        const std::string recError = recorder_.lastError();
        if (!recError.empty()) {
            stopRecording();
        } else {
            uint32_t secs = static_cast<uint32_t>(recorder_.durationSec());
            ui_->setRecordState(true, secs);
        }
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
    cfg.audioDevice = audioDevice_;
    cfg.audioChannels = 1;
    cfg.audioSampleRate = kLiveAudioSampleRate;
    cfg.audioBitrate = 64000;
    cfg.useExternalAudio = true;

    if (!startMicrophoneIfNeeded()) {
        ui_->setStatusText("录像无法启动麦克风: " + microphone_.lastError(), 0xff5555);
        return;
    }

    if (!recorder_.start(cfg)) {
        stopMicrophoneIfUnused();
        ui_->setStatusText("录像失败: " + recorder_.lastError(), 0xff5555);
        return;
    }
    recording_.store(true);
    ui_->setRecordButtons(true);
    ui_->setRecordState(true, 0);
    ui_->setStatusText("录像已开始（视频 + 麦克风）", 0x66cc66);
}

void AppController::stopRecording() {
    if (!recording_.load()) return;
    recorder_.stop();
    recording_.store(false);
    stopMicrophoneIfUnused();
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

void AppController::openAlbum() {
    if (!ui_) return;
    // 浏览相册或播放录像时释放摄像头，避免采集、录像线程和播放器同时抢占资源。
    if (previewing_.load() || camera_.isOpen()) stopPreview();
    refreshFileList();
}

void AppController::closeAlbum() {
    // 相册关闭后保持相机停止状态，由用户按需重新开始预览。
}

void AppController::refreshFileList() {
    const std::string wantedPath = selectedFilePath_;
    files_ = filemanager::FileManager::list(recordingsDir_);
    selectedFile_ = -1;
    if (!wantedPath.empty()) {
        auto it = std::find_if(files_.begin(), files_.end(), [&wantedPath](const auto &f) {
            return f.path == wantedPath;
        });
        if (it != files_.end()) selectedFile_ = static_cast<int>(it - files_.begin());
    }
    if (selectedFile_ < 0) selectedFilePath_.clear();
    if (!ui_) return;
    std::vector<std::vector<std::string>> rows;
    rows.reserve(files_.size());
    for (const auto &f : files_) {
        rows.push_back({f.name, filemanager::FileManager::formatSize(f.size), f.created});
    }
    ui_->setAlbumFiles(rows);
    ui_->setAlbumSelection(selectedFile_);
}

void AppController::selectFile(int row) {
    selectedFile_ = row;
    if (row >= 0 && row < static_cast<int>(files_.size())) {
        selectedFilePath_ = files_[row].path;
    } else {
        selectedFilePath_.clear();
    }
    if (ui_) {
        ui_->setTransferEnabled(row >= 0 && row < static_cast<int>(files_.size()));
    }
}

void AppController::playSelectedFile() {
    if (!ui_) return;
    if (playbackPid_ > 0) {
        ui_->setAlbumHint("已有视频正在播放", 0xffaa44);
        return;
    }
    if (transfer_.isTransferring()) {
        ui_->setAlbumHint("传输中，暂不能播放", 0xffaa44);
        return;
    }
    if (selectedFile_ < 0 || selectedFile_ >= static_cast<int>(files_.size())) {
        ui_->setAlbumHint("请先单击选中录像", 0xffaa44);
        return;
    }

    const std::string path = files_[selectedFile_].path;
    if (previewing_.load() || camera_.isOpen()) stopPreview();

#ifdef CAMERA_LVGL_BOARD
    // 相册保存的是 recordings/... 相对路径，不能直接拼到 file:// 后。
    // 在 fork 前解析及编码，文件被移走时直接反馈错误，不启动播放器。
    const std::string fileUri = fscompat::fileUri(path);
    if (fileUri.empty()) {
        ui_->setAlbumHint("无法打开录像: " + std::string(std::strerror(errno)), 0xff5555);
        ui_->setStatusText("录像文件不可用", 0xff5555);
        return;
    }
    const std::string uri = "uri=" + fileUri;
    // 640x480 视频用黑边居中到 1024x600，避免软件放大消耗有限的 CPU。
    const std::string videoSinkOption =
        "video-sink=videoconvert ! videobox left=-192 right=-192 top=-60 bottom=-60 fill=black ! "
        "video/x-raw,width=1024,height=600 ! fbdevsink device=/dev/fb0";
    const std::string audioSinkOption = "audio-sink=alsasink device=" + audioDevice_;
    const pid_t pid = ::fork();
    if (pid == 0) {
        int logFd = ::open("/tmp/camera_playback.log", O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (logFd >= 0) {
            ::dup2(logFd, STDOUT_FILENO);
            ::dup2(logFd, STDERR_FILENO);
            if (logFd > STDERR_FILENO) ::close(logFd);
        }

        ::execl("/usr/bin/gst-launch-1.0", "gst-launch-1.0", "-q",
                "playbin", uri.c_str(), videoSinkOption.c_str(),
                audioSinkOption.c_str(), static_cast<char *>(nullptr));
        ::perror("启动 gst-launch-1.0 失败");
        ::_exit(127);
    }
#else
    const pid_t pid = ::fork();
    if (pid == 0) {
        int logFd = ::open("/tmp/camera_playback.log", O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (logFd >= 0) {
            ::dup2(logFd, STDOUT_FILENO);
            ::dup2(logFd, STDERR_FILENO);
            if (logFd > STDERR_FILENO) ::close(logFd);
        }
        ::execlp("gst-play-1.0", "gst-play-1.0", "-q", "--no-interactive",
                 path.c_str(), static_cast<char *>(nullptr));
        ::_exit(127);
    }
#endif

    if (pid < 0) {
        playbackPid_ = -1;
        ui_->setAlbumHint("播放器启动失败", 0xff5555);
        ui_->setStatusText("播放器启动失败", 0xff5555);
        return;
    }

    playbackPid_ = static_cast<int>(pid);
    ui_->setAlbumHint("正在全屏播放: " + files_[selectedFile_].name, 0x66cc66);
    ui_->setStatusText("正在播放录像", 0x66cc66);
}

void AppController::pollPlayback() {
    if (playbackPid_ <= 0 || !ui_) return;

    int status = 0;
    const pid_t result = ::waitpid(static_cast<pid_t>(playbackPid_), &status, WNOHANG);
    if (result == 0) return;
    if (result < 0 && errno != ECHILD) return;

    playbackPid_ = -1;
    const bool success = result > 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0;
    if (success) {
        ui_->setAlbumHint("播放结束，单击选中，双击播放", 0x66cc66);
        ui_->setStatusText("录像播放结束", 0x66cc66);
    } else {
        ui_->setAlbumHint("播放失败，请查看 /tmp/camera_playback.log", 0xff5555);
        ui_->setStatusText("录像播放失败", 0xff5555);
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
#ifdef CAMERA_LVGL_BOARD
    const bool started = transfer_.startTcp(src, transferHost_, kTransferPort);
#else
    const bool started = transfer_.start(src, receivedDir_);
#endif
    if (!started) {
        auto r = transfer_.takeResult();
        ui_->setTransferStatus("失败: " + r.error, 0xff5555);
        ui_->setStatusText("传输失败", 0xff5555);
        return;
    }

    ui_->setTransferProgress(0);
    ui_->setTransferStatus("传输中...", 0xcccccc);
    ui_->setStatusText("传输已开始", 0x66cc66);
}
