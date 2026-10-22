#pragma once

#include "lvgl.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

/** 面向应用的 LVGL 界面封装。所有方法都应在 GUI 线程调用。 */
class Ui {
public:
    Ui() = default;
    ~Ui() = default;

    void init(lv_display_t *disp, const lv_font_t *fontLarge);

    // 相机与文件选择回调
    std::function<void(int)> onCameraSelect;
    std::function<void()> onStartPreview;
    std::function<void()> onStopPreview;
    std::function<void()> onRecordToggle;
    std::function<void()> onAlbumOpen;
    std::function<void()> onAlbumClose;
    std::function<void()> onTransfer;
    std::function<void(int)> onFileSelect;
    std::function<void(int)> onFileActivate;

    // 状态展示
    void setCameraList(const std::vector<std::string> &paths, int selected);
    void setStatusText(const std::string &text, uint32_t color = 0xffffff);
    void setRecordState(bool recording, uint32_t seconds);
    void setSaveDirText(const std::string &text);
    void setPreviewInfoText(const std::string &text);
    void setAlbumFiles(const std::vector<std::vector<std::string>> &rows);
    void setAlbumSelection(int row);
    void setAlbumVisible(bool visible);
    void setAlbumHint(const std::string &text, uint32_t color = 0x999999);
    bool albumVisible() const;
    void setTransferProgress(int percent);
    void setTransferStatus(const std::string &text, uint32_t color = 0xcccccc);

    // 预览帧
    void setPreviewFrame(const uint8_t *rgb565, uint32_t w, uint32_t h);
    void clearPreview();

    // 取值
    int selectedCameraIndex() const;
    int selectedFileIndex() const;
    void setTransferEnabled(bool enabled);
    void setRecordButtons(bool recording);

private:
    void handleAlbumItemClick(int index);

    const lv_font_t *fontLarge_ = nullptr;
    lv_obj_t *screen_ = nullptr;
    lv_obj_t *cameraDropdown_ = nullptr;
    lv_obj_t *previewBtn_ = nullptr;
    lv_obj_t *stopPreviewBtn_ = nullptr;
    lv_obj_t *recordBtn_ = nullptr;
    lv_obj_t *albumBtn_ = nullptr;
    lv_obj_t *albumPage_ = nullptr;
    lv_obj_t *albumList_ = nullptr;
    lv_obj_t *albumEmptyLabel_ = nullptr;
    lv_obj_t *albumHintLabel_ = nullptr;
    lv_obj_t *albumSelectionLabel_ = nullptr;
    lv_obj_t *recordStateLabel_ = nullptr;
    lv_obj_t *saveDirLabel_ = nullptr;
    lv_obj_t *statusLabel_ = nullptr;
    lv_obj_t *previewImage_ = nullptr;
    lv_obj_t *previewInfo_ = nullptr;
    lv_obj_t *transferBtn_ = nullptr;
    lv_obj_t *transferBar_ = nullptr;
    lv_obj_t *transferStatus_ = nullptr;

    lv_image_dsc_t imageDsc_{};
    std::vector<lv_obj_t *> albumCards_;
    std::vector<std::vector<std::string>> albumRows_;
    bool imageValid_ = false;
    int selectedFile_ = -1;
    int lastClickedFile_ = -1;
    uint32_t lastClickTick_ = 0;
};
