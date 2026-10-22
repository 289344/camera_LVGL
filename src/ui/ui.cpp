#include "ui/ui.h"

#include <cstdint>
#include <cstdio>

namespace {

constexpr int kWinW = 1024;
constexpr int kWinH = 600;
constexpr int kTopH = 44;
constexpr int kPanelW = 280;
constexpr int kCenterW = kWinW - kPanelW;
constexpr uint32_t kDoubleClickMs = 450;

void onCameraDropdown(lv_event_t *e) {
    auto *ui = static_cast<Ui *>(lv_event_get_user_data(e));
    if (ui->onCameraSelect) {
        ui->onCameraSelect(static_cast<int>(lv_dropdown_get_selected(
            static_cast<lv_obj_t *>(lv_event_get_target(e)))));
    }
}

}  // namespace

void Ui::init(lv_display_t *disp, const lv_font_t *fontLarge) {
    (void)disp;
    fontLarge_ = fontLarge;
    screen_ = lv_screen_active();
    lv_obj_set_style_bg_color(screen_, lv_color_hex(0x16161e), 0);
    lv_obj_set_style_pad_all(screen_, 0, 0);

    // ---- 顶栏 ----
    lv_obj_t *top = lv_obj_create(screen_);
    lv_obj_set_size(top, kWinW, kTopH);
    lv_obj_align(top, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(top, lv_color_hex(0x222230), 0);
    lv_obj_set_style_radius(top, 0, 0);
    lv_obj_set_style_border_width(top, 0, 0);
    lv_obj_set_style_pad_all(top, 0, 0);

    lv_obj_t *title = lv_label_create(top);
    lv_label_set_text(title, "相机控制");
    if (fontLarge_) lv_obj_set_style_text_font(title, fontLarge_, 0);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 12, 0);

    statusLabel_ = lv_label_create(top);
    lv_label_set_text(statusLabel_, "就绪");
    lv_obj_align(statusLabel_, LV_ALIGN_RIGHT_MID, -12, 0);

    // ---- 左侧控制面板 ----
    lv_obj_t *left = lv_obj_create(screen_);
    lv_obj_set_size(left, kPanelW, kWinH - kTopH);
    lv_obj_align(left, LV_ALIGN_TOP_LEFT, 0, kTopH);
    lv_obj_set_style_bg_color(left, lv_color_hex(0x1e1e2a), 0);
    lv_obj_set_style_radius(left, 0, 0);
    lv_obj_set_style_border_width(left, 0, 0);
    lv_obj_set_style_pad_all(left, 10, 0);

    int y = 6;
    lv_obj_t *camLabel = lv_label_create(left);
    lv_label_set_text(camLabel, "摄像头");
    lv_obj_align(camLabel, LV_ALIGN_TOP_LEFT, 0, y);

    cameraDropdown_ = lv_dropdown_create(left);
    lv_obj_set_size(cameraDropdown_, kPanelW - 20, 40);
    lv_obj_align(cameraDropdown_, LV_ALIGN_TOP_LEFT, 0, y + 24);
    lv_obj_add_event_cb(cameraDropdown_, onCameraDropdown, LV_EVENT_VALUE_CHANGED, this);

    y += 82;
    previewBtn_ = lv_button_create(left);
    lv_obj_set_size(previewBtn_, kPanelW - 20, 44);
    lv_obj_align(previewBtn_, LV_ALIGN_TOP_LEFT, 0, y);
    lv_obj_t *p1 = lv_label_create(previewBtn_);
    lv_label_set_text(p1, "开始预览");
    lv_obj_center(p1);
    lv_obj_add_event_cb(previewBtn_, [](lv_event_t *e) {
        auto *ui = static_cast<Ui *>(lv_event_get_user_data(e));
        if (ui->onStartPreview) ui->onStartPreview();
    }, LV_EVENT_CLICKED, this);

    y += 54;
    stopPreviewBtn_ = lv_button_create(left);
    lv_obj_set_size(stopPreviewBtn_, kPanelW - 20, 44);
    lv_obj_align(stopPreviewBtn_, LV_ALIGN_TOP_LEFT, 0, y);
    lv_obj_t *p2 = lv_label_create(stopPreviewBtn_);
    lv_label_set_text(p2, "停止预览");
    lv_obj_center(p2);
    lv_obj_add_event_cb(stopPreviewBtn_, [](lv_event_t *e) {
        auto *ui = static_cast<Ui *>(lv_event_get_user_data(e));
        if (ui->onStopPreview) ui->onStopPreview();
    }, LV_EVENT_CLICKED, this);

    y += 64;
    recordBtn_ = lv_button_create(left);
    lv_obj_set_size(recordBtn_, kPanelW - 20, 44);
    lv_obj_align(recordBtn_, LV_ALIGN_TOP_LEFT, 0, y);
    lv_obj_t *r1 = lv_label_create(recordBtn_);
    lv_label_set_text(r1, "开始录像");
    lv_obj_center(r1);
    lv_obj_add_event_cb(recordBtn_, [](lv_event_t *e) {
        auto *ui = static_cast<Ui *>(lv_event_get_user_data(e));
        if (ui->onRecordToggle) ui->onRecordToggle();
    }, LV_EVENT_CLICKED, this);

    y += 64;
    recordStateLabel_ = lv_label_create(left);
    lv_label_set_text(recordStateLabel_, "未录像");
    lv_obj_set_style_text_color(recordStateLabel_, lv_color_hex(0xaaaaaa), 0);
    lv_obj_align(recordStateLabel_, LV_ALIGN_TOP_LEFT, 0, y);

    y += 30;
    lv_obj_t *dirTitle = lv_label_create(left);
    lv_label_set_text(dirTitle, "保存目录:");
    lv_obj_align(dirTitle, LV_ALIGN_TOP_LEFT, 0, y);

    saveDirLabel_ = lv_label_create(left);
    lv_label_set_text(saveDirLabel_, "未设置");
    lv_label_set_long_mode(saveDirLabel_, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(saveDirLabel_, kPanelW - 20);
    lv_obj_align(saveDirLabel_, LV_ALIGN_TOP_LEFT, 0, y + 20);

    networkPreviewBtn_ = lv_button_create(left);
    lv_obj_set_size(networkPreviewBtn_, kPanelW - 20, 40);
    lv_obj_align(networkPreviewBtn_, LV_ALIGN_TOP_LEFT, 0, 340);
    lv_obj_set_style_bg_color(networkPreviewBtn_, lv_color_hex(0x3949AB), 0);
    lv_obj_t *networkLabel = lv_label_create(networkPreviewBtn_);
    lv_label_set_text(networkLabel, "网络预览: 关");
    lv_obj_center(networkLabel);
    lv_obj_add_event_cb(networkPreviewBtn_, [](lv_event_t *e) {
        auto *ui = static_cast<Ui *>(lv_event_get_user_data(e));
        if (ui->onNetworkToggle) ui->onNetworkToggle();
    }, LV_EVENT_CLICKED, this);

    albumBtn_ = lv_button_create(left);
    lv_obj_set_size(albumBtn_, kPanelW - 20, 48);
    lv_obj_align(albumBtn_, LV_ALIGN_TOP_LEFT, 0, 390);
    lv_obj_set_style_bg_color(albumBtn_, lv_color_hex(0x6750A4), 0);
    lv_obj_t *albumLabel = lv_label_create(albumBtn_);
    lv_label_set_text(albumLabel, "相册");
    if (fontLarge_) lv_obj_set_style_text_font(albumLabel, fontLarge_, 0);
    lv_obj_center(albumLabel);
    lv_obj_add_event_cb(albumBtn_, [](lv_event_t *e) {
        auto *ui = static_cast<Ui *>(lv_event_get_user_data(e));
        ui->setAlbumVisible(true);
        if (ui->onAlbumOpen) ui->onAlbumOpen();
    }, LV_EVENT_CLICKED, this);

    // ---- 中央预览区 ----
    lv_obj_t *center = lv_obj_create(screen_);
    lv_obj_set_size(center, kCenterW, kWinH - kTopH);
    lv_obj_align(center, LV_ALIGN_TOP_LEFT, kPanelW, kTopH);
    lv_obj_set_style_bg_color(center, lv_color_hex(0x0b0b10), 0);
    lv_obj_set_style_radius(center, 0, 0);
    lv_obj_set_style_border_width(center, 0, 0);
    lv_obj_set_style_pad_all(center, 0, 0);

    previewImage_ = lv_image_create(center);
    lv_obj_align(previewImage_, LV_ALIGN_CENTER, 0, -8);
    lv_obj_add_flag(previewImage_, LV_OBJ_FLAG_HIDDEN);

    previewInfo_ = lv_label_create(center);
    lv_label_set_text(previewInfo_, "无预览");
    lv_obj_set_style_text_color(previewInfo_, lv_color_hex(0x666666), 0);
    lv_obj_center(previewInfo_);

    // ---- 全屏相册页 ----
    albumPage_ = lv_obj_create(screen_);
    lv_obj_set_size(albumPage_, kWinW, kWinH);
    lv_obj_align(albumPage_, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(albumPage_, lv_color_hex(0x12121a), 0);
    lv_obj_set_style_radius(albumPage_, 0, 0);
    lv_obj_set_style_border_width(albumPage_, 0, 0);
    lv_obj_set_style_pad_all(albumPage_, 0, 0);

    lv_obj_t *albumTop = lv_obj_create(albumPage_);
    lv_obj_set_size(albumTop, kWinW, 64);
    lv_obj_align(albumTop, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(albumTop, lv_color_hex(0x222230), 0);
    lv_obj_set_style_radius(albumTop, 0, 0);
    lv_obj_set_style_border_width(albumTop, 0, 0);
    lv_obj_set_style_pad_all(albumTop, 0, 0);

    lv_obj_t *backBtn = lv_button_create(albumTop);
    lv_obj_set_size(backBtn, 104, 42);
    lv_obj_align(backBtn, LV_ALIGN_LEFT_MID, 16, 0);
    lv_obj_t *backLabel = lv_label_create(backBtn);
    lv_label_set_text(backLabel, "返回");
    lv_obj_center(backLabel);
    lv_obj_add_event_cb(backBtn, [](lv_event_t *e) {
        auto *ui = static_cast<Ui *>(lv_event_get_user_data(e));
        ui->setAlbumVisible(false);
        if (ui->onAlbumClose) ui->onAlbumClose();
    }, LV_EVENT_CLICKED, this);

    lv_obj_t *albumTitle = lv_label_create(albumTop);
    lv_label_set_text(albumTitle, "录像相册");
    if (fontLarge_) lv_obj_set_style_text_font(albumTitle, fontLarge_, 0);
    lv_obj_align(albumTitle, LV_ALIGN_CENTER, 0, 0);

    albumHintLabel_ = lv_label_create(albumPage_);
    lv_label_set_text(albumHintLabel_, "单击选中，双击播放");
    lv_obj_set_style_text_color(albumHintLabel_, lv_color_hex(0x999999), 0);
    lv_obj_align(albumHintLabel_, LV_ALIGN_TOP_LEFT, 22, 76);

    albumList_ = lv_obj_create(albumPage_);
    lv_obj_set_size(albumList_, kWinW - 44, 370);
    lv_obj_align(albumList_, LV_ALIGN_TOP_LEFT, 22, 104);
    lv_obj_set_style_bg_color(albumList_, lv_color_hex(0x181823), 0);
    lv_obj_set_style_border_width(albumList_, 0, 0);
    lv_obj_set_style_radius(albumList_, 10, 0);
    lv_obj_set_style_pad_all(albumList_, 5, 0);
    lv_obj_set_style_pad_row(albumList_, 8, 0);
    lv_obj_set_flex_flow(albumList_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(albumList_, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(albumList_, LV_SCROLLBAR_MODE_AUTO);

    albumEmptyLabel_ = lv_label_create(albumList_);
    lv_label_set_text(albumEmptyLabel_, "暂无录像");
    lv_obj_set_style_text_color(albumEmptyLabel_, lv_color_hex(0x777777), 0);
    if (fontLarge_) lv_obj_set_style_text_font(albumEmptyLabel_, fontLarge_, 0);
    lv_obj_align(albumEmptyLabel_, LV_ALIGN_TOP_MID, 0, 120);

    albumSelectionLabel_ = lv_label_create(albumPage_);
    lv_label_set_text(albumSelectionLabel_, "未选择视频");
    lv_obj_set_style_text_color(albumSelectionLabel_, lv_color_hex(0xbbbbbb), 0);
    lv_label_set_long_mode(albumSelectionLabel_, LV_LABEL_LONG_DOT);
    lv_obj_set_width(albumSelectionLabel_, 760);
    lv_obj_align(albumSelectionLabel_, LV_ALIGN_TOP_LEFT, 22, 482);

    transferStatus_ = lv_label_create(albumPage_);
    lv_label_set_text(transferStatus_, "未传输");
    lv_obj_set_style_text_color(transferStatus_, lv_color_hex(0x888888), 0);
    lv_label_set_long_mode(transferStatus_, LV_LABEL_LONG_DOT);
    lv_obj_set_width(transferStatus_, 780);
    lv_obj_align(transferStatus_, LV_ALIGN_TOP_LEFT, 22, 510);

    transferBar_ = lv_bar_create(albumPage_);
    lv_obj_set_size(transferBar_, 780, 12);
    lv_obj_align(transferBar_, LV_ALIGN_TOP_LEFT, 22, 542);
    lv_bar_set_range(transferBar_, 0, 100);
    lv_bar_set_value(transferBar_, 0, LV_ANIM_OFF);

    transferBtn_ = lv_button_create(albumPage_);
    lv_obj_set_size(transferBtn_, 180, 48);
    lv_obj_align(transferBtn_, LV_ALIGN_BOTTOM_RIGHT, -22, -26);
    lv_obj_t *t1 = lv_label_create(transferBtn_);
    lv_label_set_text(t1, "传输到服务器");
    lv_obj_center(t1);
    lv_obj_add_event_cb(transferBtn_, [](lv_event_t *e) {
        auto *ui = static_cast<Ui *>(lv_event_get_user_data(e));
        if (ui->onTransfer) ui->onTransfer();
    }, LV_EVENT_CLICKED, this);

    lv_obj_add_flag(albumPage_, LV_OBJ_FLAG_HIDDEN);
    setTransferEnabled(false);
}

void Ui::refreshAll() {
    if (!screen_) return;
    lv_area_t fullArea = {0, 0, kWinW - 1, kWinH - 1};
    lv_obj_invalidate_area(screen_, &fullArea);
}

void Ui::setCameraList(const std::vector<std::string> &paths, int selected) {
    if (!cameraDropdown_) return;
    std::string options;
    for (const auto &p : paths) {
        if (!options.empty()) options += "\n";
        options += p;
    }
    lv_dropdown_set_options(cameraDropdown_, options.c_str());
    if (selected >= 0 && selected < static_cast<int>(paths.size())) {
        lv_dropdown_set_selected(cameraDropdown_, selected);
    }
}

void Ui::setStatusText(const std::string &text, uint32_t color) {
    if (!statusLabel_) return;
    lv_label_set_text(statusLabel_, text.c_str());
    lv_obj_set_style_text_color(statusLabel_, lv_color_hex(color), 0);
}

void Ui::setRecordState(bool recording, uint32_t seconds) {
    if (!recordStateLabel_) return;
    char buf[64];
    if (recording) {
        std::snprintf(buf, sizeof(buf), "录像中 %02u:%02u", seconds / 60, seconds % 60);
        lv_label_set_text(recordStateLabel_, buf);
        lv_obj_set_style_text_color(recordStateLabel_, lv_color_hex(0xff5555), 0);
    } else {
        lv_label_set_text(recordStateLabel_, "未录像");
        lv_obj_set_style_text_color(recordStateLabel_, lv_color_hex(0xaaaaaa), 0);
    }
}

void Ui::setSaveDirText(const std::string &text) {
    if (saveDirLabel_) lv_label_set_text(saveDirLabel_, text.c_str());
}

void Ui::setPreviewInfoText(const std::string &text) {
    if (previewInfo_) lv_label_set_text(previewInfo_, text.c_str());
}

void Ui::setNetworkPreviewState(bool active, bool connected) {
    if (!networkPreviewBtn_) return;
    lv_obj_t *label = lv_obj_get_child(networkPreviewBtn_, 0);
    if (!label) return;

    const int state = !active ? 0 : (connected ? 2 : 1);
    if (state == networkUiState_) return;
    networkUiState_ = state;

    if (!active) {
        lv_label_set_text(label, "网络预览: 关");
        lv_obj_set_style_bg_color(networkPreviewBtn_, lv_color_hex(0x3949AB), 0);
        refreshAll();
        return;
    }

    if (connected) {
        lv_label_set_text(label, "网络预览: 直播中");
    } else {
        lv_label_set_text(label, "网络预览: 连接中");
    }
    lv_obj_set_style_bg_color(networkPreviewBtn_,
                              lv_color_hex(connected ? 0xC62828 : 0x8D6E63), 0);

    // 状态切换时强制重绘整棵 UI 树，避免板端 framebuffer 留下旧画面或只显示
    // 发生变化的按钮。正常预览帧仍只刷新中央图像，不影响帧率。
    refreshAll();
}

void Ui::setAlbumFiles(const std::vector<std::vector<std::string>> &rows) {
    if (!albumList_ || albumRows_ == rows) return;
    albumRows_ = rows;
    lv_obj_clean(albumList_);
    albumCards_.clear();
    albumEmptyLabel_ = nullptr;

    if (rows.empty()) {
        selectedFile_ = -1;
        albumEmptyLabel_ = lv_label_create(albumList_);
        lv_label_set_text(albumEmptyLabel_, "暂无录像");
        lv_obj_set_style_text_color(albumEmptyLabel_, lv_color_hex(0x777777), 0);
        if (fontLarge_) lv_obj_set_style_text_font(albumEmptyLabel_, fontLarge_, 0);
        lv_obj_align(albumEmptyLabel_, LV_ALIGN_TOP_MID, 0, 120);
        setAlbumSelection(-1);
        return;
    }

    for (size_t i = 0; i < rows.size(); ++i) {
        const auto &row = rows[i];
        lv_obj_t *card = lv_button_create(albumList_);
        lv_obj_set_size(card, lv_pct(100), 82);
        lv_obj_set_style_bg_color(card, lv_color_hex(0x20202c), 0);
        lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(card, 1, 0);
        lv_obj_set_style_border_color(card, lv_color_hex(0x343446), 0);
        lv_obj_set_style_radius(card, 8, 0);
        lv_obj_set_style_pad_all(card, 0, 0);
        lv_obj_set_user_data(card, reinterpret_cast<void *>(static_cast<intptr_t>(i + 1)));
        lv_obj_add_event_cb(card, [](lv_event_t *e) {
            auto *ui = static_cast<Ui *>(lv_event_get_user_data(e));
            auto *target = static_cast<lv_obj_t *>(lv_event_get_target(e));
            int index = static_cast<int>(reinterpret_cast<intptr_t>(lv_obj_get_user_data(target))) - 1;
            ui->handleAlbumItemClick(index);
        }, LV_EVENT_CLICKED, this);
        albumCards_.push_back(card);

        lv_obj_t *icon = lv_obj_create(card);
        lv_obj_set_size(icon, 58, 58);
        lv_obj_align(icon, LV_ALIGN_LEFT_MID, 14, 0);
        lv_obj_set_style_bg_color(icon, lv_color_hex(0x3949AB), 0);
        lv_obj_set_style_border_width(icon, 0, 0);
        lv_obj_set_style_radius(icon, 8, 0);
        lv_obj_clear_flag(icon, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_t *iconLabel = lv_label_create(icon);
        lv_label_set_text(iconLabel, "视频");
        lv_obj_center(iconLabel);

        const char *name = row.size() > 0 ? row[0].c_str() : "";
        const char *size = row.size() > 1 ? row[1].c_str() : "";
        const char *created = row.size() > 2 ? row[2].c_str() : "";

        lv_obj_t *nameLabel = lv_label_create(card);
        lv_label_set_text(nameLabel, name);
        lv_label_set_long_mode(nameLabel, LV_LABEL_LONG_DOT);
        lv_obj_set_width(nameLabel, 590);
        if (fontLarge_) lv_obj_set_style_text_font(nameLabel, fontLarge_, 0);
        lv_obj_align(nameLabel, LV_ALIGN_TOP_LEFT, 88, 12);

        lv_obj_t *timeLabel = lv_label_create(card);
        lv_label_set_text(timeLabel, created);
        lv_obj_set_style_text_color(timeLabel, lv_color_hex(0x999999), 0);
        lv_obj_align(timeLabel, LV_ALIGN_TOP_LEFT, 88, 46);

        lv_obj_t *sizeLabel = lv_label_create(card);
        lv_label_set_text(sizeLabel, size);
        lv_obj_set_style_text_color(sizeLabel, lv_color_hex(0xaaaaaa), 0);
        lv_obj_align(sizeLabel, LV_ALIGN_TOP_RIGHT, -18, 16);

        lv_obj_t *hintLabel = lv_label_create(card);
        lv_label_set_text(hintLabel, "双击播放");
        lv_obj_set_style_text_color(hintLabel, lv_color_hex(0x6f7bd9), 0);
        lv_obj_align(hintLabel, LV_ALIGN_BOTTOM_RIGHT, -18, -14);
    }

    if (selectedFile_ >= static_cast<int>(albumRows_.size())) selectedFile_ = -1;
    setAlbumSelection(selectedFile_);
}

void Ui::setAlbumSelection(int row) {
    selectedFile_ = row;
    const bool valid = row >= 0 && row < static_cast<int>(albumCards_.size());
    for (size_t i = 0; i < albumCards_.size(); ++i) {
        const bool selected = valid && static_cast<int>(i) == row;
        lv_obj_set_style_border_width(albumCards_[i], selected ? 3 : 1, 0);
        lv_obj_set_style_border_color(albumCards_[i],
            lv_color_hex(selected ? 0x64B5F6 : 0x343446), 0);
        lv_obj_set_style_bg_color(albumCards_[i],
            lv_color_hex(selected ? 0x26344D : 0x20202C), 0);
    }

    if (albumSelectionLabel_) {
        if (valid && row < static_cast<int>(albumRows_.size()) && !albumRows_[row].empty()) {
            lv_label_set_text(albumSelectionLabel_,
                ("已选择: " + albumRows_[row][0]).c_str());
        } else {
            lv_label_set_text(albumSelectionLabel_, "未选择视频");
        }
    }
    setTransferEnabled(valid);
}

void Ui::setAlbumVisible(bool visible) {
    if (!albumPage_) return;
    if (visible) {
        lv_obj_move_foreground(albumPage_);
        lv_obj_remove_flag(albumPage_, LV_OBJ_FLAG_HIDDEN);
        lastClickedFile_ = -1;
    } else {
        lv_obj_add_flag(albumPage_, LV_OBJ_FLAG_HIDDEN);
        lastClickedFile_ = -1;
    }
    refreshAll();
}

void Ui::setAlbumHint(const std::string &text, uint32_t color) {
    if (!albumHintLabel_) return;
    lv_label_set_text(albumHintLabel_, text.c_str());
    lv_obj_set_style_text_color(albumHintLabel_, lv_color_hex(color), 0);
}

bool Ui::albumVisible() const {
    return albumPage_ && !lv_obj_has_flag(albumPage_, LV_OBJ_FLAG_HIDDEN);
}

void Ui::handleAlbumItemClick(int index) {
    if (index < 0 || index >= static_cast<int>(albumRows_.size())) return;
    setAlbumSelection(index);
    if (onFileSelect) onFileSelect(index);

    const uint32_t now = lv_tick_get();
    const bool isDoubleClick = lastClickedFile_ == index &&
                               static_cast<uint32_t>(now - lastClickTick_) <= kDoubleClickMs;
    if (isDoubleClick) {
        lastClickedFile_ = -1;
        lastClickTick_ = 0;
        if (onFileActivate) onFileActivate(index);
    } else {
        lastClickedFile_ = index;
        lastClickTick_ = now;
    }
}

void Ui::setTransferProgress(int percent) {
    if (!transferBar_) return;
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    lv_bar_set_value(transferBar_, percent, LV_ANIM_OFF);
}

void Ui::setTransferStatus(const std::string &text, uint32_t color) {
    if (!transferStatus_) return;
    lv_label_set_text(transferStatus_, text.c_str());
    lv_obj_set_style_text_color(transferStatus_, lv_color_hex(color), 0);
}

void Ui::setPreviewFrame(const uint8_t *rgb565, uint32_t w, uint32_t h) {
    if (!previewImage_ || !rgb565 || w == 0 || h == 0) return;
    imageDsc_.header.magic = LV_IMAGE_HEADER_MAGIC;
    imageDsc_.header.cf = LV_COLOR_FORMAT_RGB565;
    imageDsc_.header.flags = 0;
    imageDsc_.header.w = static_cast<uint16_t>(w);
    imageDsc_.header.h = static_cast<uint16_t>(h);
    imageDsc_.header.stride = static_cast<uint16_t>(w * 2);
    imageDsc_.header.reserved_2 = 0;
    imageDsc_.data_size = w * h * 2;
    imageDsc_.data = rgb565;
    imageDsc_.reserved = nullptr;
    imageDsc_.reserved_2 = nullptr;
    lv_image_set_src(previewImage_, &imageDsc_);
    // 等比缩放以适配中央预览区。
    const uint32_t areaW = kCenterW - 20, areaH = kWinH - kTopH - 20;
    uint32_t zoom = LV_SCALE_NONE;
    uint32_t zw = areaW * 256 / w;
    uint32_t zh = areaH * 256 / h;
    zoom = zw < zh ? zw : zh;
    if (zoom == 0) zoom = 1;
    if (zoom > LV_SCALE_NONE) zoom = LV_SCALE_NONE;
    lv_image_set_scale(previewImage_, zoom);
    imageValid_ = true;
    lv_obj_remove_flag(previewImage_, LV_OBJ_FLAG_HIDDEN);
}

void Ui::clearPreview() {
    if (previewImage_) lv_obj_add_flag(previewImage_, LV_OBJ_FLAG_HIDDEN);
    if (previewInfo_) lv_label_set_text(previewInfo_, "无预览");
    imageValid_ = false;
}

void Ui::setTransferEnabled(bool enabled) {
    if (!transferBtn_) return;
    if (enabled) lv_obj_remove_state(transferBtn_, LV_STATE_DISABLED);
    else lv_obj_add_state(transferBtn_, LV_STATE_DISABLED);
}

void Ui::setRecordButtons(bool recording) {
    if (!recordBtn_) return;

    const bool changed = !recordButtonStateInitialized_ || recording != recordButtonState_;
    recordButtonStateInitialized_ = true;
    recordButtonState_ = recording;
    if (!changed) return;

    lv_obj_t *label = lv_obj_get_child(recordBtn_, 0);
    if (label) lv_label_set_text(label, recording ? "停止录像" : "开始录像");
    // 录像状态切换是用户可见的关键状态；整屏重绘可修复板端静态控件
    // 在外部 framebuffer 操作后消失的问题。
    refreshAll();
}

int Ui::selectedCameraIndex() const {
    return cameraDropdown_ ? static_cast<int>(lv_dropdown_get_selected(cameraDropdown_)) : -1;
}

int Ui::selectedFileIndex() const {
    return selectedFile_;
}
