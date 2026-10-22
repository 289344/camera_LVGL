#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "camera/camera_device.h"
#include "preview/frame_pool.h"
#include "recording/recording_service.h"
#include "file_manager/file_manager.h"
#include "file_transfer/file_transfer_service.h"

class Ui;

/** 协调 UI 与各业务模块。 */
class AppController {
public:
    AppController();
    ~AppController();

    void init(Ui *ui);

    // 相机
    const std::vector<std::string> &cameraList() const { return cameras_; }
    void selectCamera(int index);
    void startPreview();
    void stopPreview();
    bool isPreviewing() const { return previewing_.load(); }

    // 录像
    void startRecording();
    void stopRecording();
    void toggleRecording();
    bool isRecording() const { return recording_.load(); }

    // 文件管理
    void openAlbum();
    void closeAlbum();
    void refreshFileList();
    void selectFile(int row);
    void playSelectedFile();
    void pollPlayback();
    bool isPlayingVideo() const { return playbackPid_ > 0; }
    const std::vector<filemanager::FileInfo> &files() const { return files_; }

    // 文件传输（后续阶段实现）
    void transferSelectedFile();

    // 由 GUI 定时器调用（主线程）
    void tick();

    const std::string &recordingsDir() const { return recordingsDir_; }
    const std::string &receivedDir() const { return receivedDir_; }

private:
    void buildCameraList();
    bool chooseFormat(camera::CameraDevice &dev, uint32_t &pixfmt, uint32_t &w, uint32_t &h);
    void handleFrame(const camera::Frame &frame);
    std::string popError();

    Ui *ui_ = nullptr;
    camera::CameraDevice camera_;
    std::unique_ptr<preview::FramePool> pool_;

    std::vector<std::string> cameras_;
    int selectedCamera_ = 0;

    std::atomic<bool> previewing_{false};
    std::atomic<bool> recording_{false};

    recording::RecordingService recorder_;
    std::vector<uint8_t> jpegEncodeBuf_;   // 仅采集线程使用

    transfer::FileTransferService transfer_;

    uint32_t previewW_ = 0;
    uint32_t previewH_ = 0;
    int displayedFrame_ = -1;

    std::mutex errMutex_;
    std::string pendingError_;

    std::string recordingsDir_;
    std::string receivedDir_;
    std::string transferHost_;

    int playbackPid_ = -1;

    std::vector<filemanager::FileInfo> files_;
    std::string selectedFilePath_;
    int selectedFile_ = -1;
    uint32_t fileRefreshCounter_ = 0;
};
