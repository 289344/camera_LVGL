#include "ui/ui.h"

#include <cstdio>

namespace {

constexpr int kWinW = 1024;
constexpr int kWinH = 600;
constexpr int kTopH = 44;
constexpr int kPanelW = 280;

void onCameraDropdown(lv_event_t *e) {
    auto *ui = static_cast<Ui *>(lv_event_get_user_data(e));
    if (ui->onCameraSelect) {
        ui->onCameraSelect(static_cast<int>(lv_dropdown_get_selected(
            static_cast<lv_obj_t *>(lv_event_get_target(e)))));
    }
}

void onFileTable(lv_event_t *e) {
    auto *ui = static_cast<Ui *>(lv_event_get_user_data(e));
    uint32_t row = 0, col = 0;
    lv_table_get_selected_cell(static_cast<lv_obj_t *>(lv_event_get_target(e)), &row, &col);
    if (ui->onFileSelect) ui->onFileSelect(static_cast<int>(row));
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
    lv_obj_t *stopP = lv_button_create(left);
    lv_obj_set_size(stopP, kPanelW - 20, 44);
    lv_obj_align(stopP, LV_ALIGN_TOP_LEFT, 0, y);
    lv_obj_t *p2 = lv_label_create(stopP);
    lv_label_set_text(p2, "停止预览");
    lv_obj_center(p2);
    lv_obj_add_event_cb(stopP, [](lv_event_t *e) {
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

    // ---- 中央预览区 ----
    lv_obj_t *center = lv_obj_create(screen_);
    lv_obj_set_size(center, kWinW - kPanelW * 2, kWinH - kTopH);
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

    // ---- 右侧文件面板 ----
    lv_obj_t *right = lv_obj_create(screen_);
    lv_obj_set_size(right, kPanelW, kWinH - kTopH);
    lv_obj_align(right, LV_ALIGN_TOP_LEFT, kWinW - kPanelW, kTopH);
    lv_obj_set_style_bg_color(right, lv_color_hex(0x1e1e2a), 0);
    lv_obj_set_style_radius(right, 0, 0);
    lv_obj_set_style_border_width(right, 0, 0);
    lv_obj_set_style_pad_all(right, 10, 0);

    lv_obj_t *fileTitle = lv_label_create(right);
    lv_label_set_text(fileTitle, "录像文件");
    if (fontLarge_) lv_obj_set_style_text_font(fileTitle, fontLarge_, 0);
    lv_obj_align(fileTitle, LV_ALIGN_TOP_LEFT, 0, 2);

    fileTable_ = lv_table_create(right);
    lv_obj_set_size(fileTable_, kPanelW - 20, 300);
    lv_obj_align(fileTable_, LV_ALIGN_TOP_LEFT, 0, 28);
    lv_table_set_col_cnt(fileTable_, 3);
    lv_table_set_col_width(fileTable_, 0, 140);
    lv_table_set_col_width(fileTable_, 1, 62);
    lv_table_set_col_width(fileTable_, 2, 58);
    lv_table_set_row_cnt(fileTable_, 1);
    lv_table_set_cell_value(fileTable_, 0, 0, "无文件");
    lv_table_set_cell_value(fileTable_, 0, 1, "");
    lv_table_set_cell_value(fileTable_, 0, 2, "");
    lv_obj_add_event_cb(fileTable_, onFileTable, LV_EVENT_VALUE_CHANGED, this);

    transferBtn_ = lv_button_create(right);
    lv_obj_set_size(transferBtn_, kPanelW - 20, 44);
    lv_obj_align(transferBtn_, LV_ALIGN_TOP_LEFT, 0, 344);
    lv_obj_t *t1 = lv_label_create(transferBtn_);
    lv_label_set_text(t1, "传输");
    lv_obj_center(t1);
    lv_obj_add_event_cb(transferBtn_, [](lv_event_t *e) {
        auto *ui = static_cast<Ui *>(lv_event_get_user_data(e));
        if (ui->onTransfer) ui->onTransfer();
    }, LV_EVENT_CLICKED, this);

    transferBar_ = lv_bar_create(right);
    lv_obj_set_size(transferBar_, kPanelW - 20, 12);
    lv_obj_align(transferBar_, LV_ALIGN_TOP_LEFT, 0, 402);
    lv_bar_set_range(transferBar_, 0, 100);
    lv_bar_set_value(transferBar_, 0, LV_ANIM_OFF);

    transferStatus_ = lv_label_create(right);
    lv_label_set_text(transferStatus_, "未传输");
    lv_obj_set_style_text_color(transferStatus_, lv_color_hex(0x888888), 0);
    lv_label_set_long_mode(transferStatus_, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(transferStatus_, kPanelW - 20);
    lv_obj_align(transferStatus_, LV_ALIGN_TOP_LEFT, 0, 422);

    setTransferEnabled(false);
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

void Ui::setFileTable(const std::vector<std::vector<std::string>> &rows) {
    if (!fileTable_) return;
    if (rows.empty()) {
        lv_table_set_row_cnt(fileTable_, 1);
        lv_table_set_cell_value(fileTable_, 0, 0, "无文件");
        lv_table_set_cell_value(fileTable_, 0, 1, "");
        lv_table_set_cell_value(fileTable_, 0, 2, "");
        selectedFile_ = -1;
        return;
    }
    lv_table_set_row_cnt(fileTable_, static_cast<uint32_t>(rows.size()));
    for (size_t r = 0; r < rows.size(); ++r) {
        for (size_t c = 0; c < rows[r].size() && c < 3; ++c) {
            lv_table_set_cell_value(fileTable_, static_cast<uint32_t>(r),
                                    static_cast<uint32_t>(c), rows[r][c].c_str());
        }
    }
    if (selectedFile_ >= static_cast<int>(rows.size())) selectedFile_ = -1;
}

void Ui::setFileTableSelection(int row) {
    if (!fileTable_) return;
    selectedFile_ = row;
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
    // 等比缩放以适配中央预览区（约 452x540）
    const uint32_t areaW = 452, areaH = 540;
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
    if (transferBtn_) {
        lv_obj_add_state(transferBtn_, LV_STATE_DISABLED);
        if (enabled) lv_obj_remove_state(transferBtn_, LV_STATE_DISABLED);
    }
}

void Ui::setRecordButtons(bool recording) {
    if (recordBtn_) {
        lv_obj_t *label = lv_obj_get_child(recordBtn_, 0);
        lv_label_set_text(label, recording ? "停止录像" : "开始录像");
    }
}

int Ui::selectedCameraIndex() const {
    return cameraDropdown_ ? static_cast<int>(lv_dropdown_get_selected(cameraDropdown_)) : -1;
}

int Ui::selectedFileIndex() const {
    return selectedFile_;
}
