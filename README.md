# camera_LVGL

基于 LVGL 的嵌入式相机控制与视频管理应用（第一阶段：Ubuntu 24.04 PC 版）。

实现功能：
- LVGL 图形界面（SDL 窗口，支持鼠标）
- 通过 V4L2 访问 `/dev/video0`、`/dev/video1`
- 摄像头实时预览（支持 MJPG / YUYV，自动像素格式转换与缩放）
- 开始 / 停止录像（摄像头 MJPEG + 麦克风 AAC，封装为 MP4）
- 录像相册（单击选中、双击全屏播放）
- 通过 TCP 将选中的录像传输到服务器（显示进度与结果）
- 板端 JPEG 帧流 + PC 浏览器实时监控（无需安装浏览器插件）

## 环境要求

- Ubuntu 24.04
- GCC/G++ 与 CMake（>= 3.16）
- SDL2 开发库：`libsdl2-dev`
- libjpeg-turbo 开发库：`libjpeg-turbo8-dev`
- FreeType 开发库：`libfreetype6-dev`（用于中文字体渲染）

安装依赖：

```bash
sudo apt-get update
sudo apt-get install -y build-essential cmake pkg-config libsdl2-dev libjpeg-turbo8-dev libfreetype6-dev
```

## 构建

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
```

## 运行

```bash
./build/camera_lvgl
```

界面操作流程：

1. 在左上角下拉框选择摄像头（`/dev/video0` 或 `/dev/video1`）。
2. 点击 **开始预览** 开始预览，点击 **停止预览** 停止预览。
3. 点击 **开始录像** 录制音视频，再次点击停止录像。
4. 录像文件默认保存到项目根目录 `recordings/`。
5. 点击左侧 **相册** 进入录像列表；单击文件进行选中，双击文件全屏播放。
6. 选中录像后点击相册右下角的 **传输到服务器**，将文件发送到 PC 接收服务。

## 目录结构

```text
camera_LVGL/
├── CMakeLists.txt
├── lv_conf.h              # LVGL 配置
├── include/               # 各模块公共头文件
│   ├── camera/            # V4L2 摄像头
│   ├── preview/           # 帧缓冲池与像素格式转换
│   ├── recording/         # 音视频 MP4 录像服务
│   ├── file_manager/      # 录像文件管理
│   ├── file_transfer/     # 本机文件传输
│   └── app/               # 应用控制器
├── src/                   # 实现
│   ├── main.cpp
│   ├── ui/                # LVGL 界面
│   ├── camera/ preview/ recording/ file_manager/ file_transfer/ app/
├── third_party/lvgl/      # LVGL 9.5（内置源码）
├── tests/                 # 独立测试程序
└── build/                 # CMake 构建产物（已 gitignore）
```

## 设计说明

- **线程模型**：GUI 线程运行 LVGL；摄像头采集线程负责取帧与像素转换；录像写入线程把 MJPEG 帧和共享麦克风 PCM 送入 FFmpeg 封装 MP4；文件传输线程负责网络发送。
- **共享麦克风**：USB 麦克风只由 `MicrophoneCapture` 打开一次，同一路 16 kHz 单声道 PCM 会按需同时分发给网络直播和 MP4 录像，避免 ALSA 报 `Device or resource busy`。
- **网络预览**：板端通过 `9002/TCP` 推送 JPEG 画面，通过 `9003/TCP` 推送原始 PCM 声音；PC 网关在 `8080/HTTP` 向浏览器提供 MJPEG 画面和 PCM 音频流。该链路只使用 Python 标准库，适合嵌入式板端保持低依赖。
- **预览帧交换**：采集线程解码到 RGB565 帧缓冲池，GUI 定时器无撕裂地取最新帧显示。
- **录像格式**：优先使用摄像头硬件 MJPEG 输出，视频不二次编码；音频编码为 AAC，封装为 MP4。若摄像头仅支持 YUYV，则软件编码为 JPEG 后再送入 FFmpeg。
- **可移植性**：摄像头、录像、文件管理、文件传输均为独立模块，便于后续移植到 i.MX6ULL Pro 时替换底层实现。

## 测试

网络预览网关端到端测试：

```bash
python3 tests/test_preview_server.py
```

该测试会同时模拟 `9002` JPEG 视频帧和 `9003` PCM 音频帧，并验证浏览器端的 `/stream.mjpg`、`/stream.pcm` 与 `/api/status`。

```bash
# 端到端音视频录像测试（无需 GUI），采集 2 秒并生成 MP4
g++ -std=c++17 -O2 -Iinclude -Isrc tests/test_record.cpp \
    src/camera/camera_device.cpp src/recording/recording_service.cpp \
    src/fs_compat.cpp -o /tmp/test_record -lpthread
/tmp/test_record
```

## 注意事项

- 录像与传输默认使用相对目录 `recordings/`、`received/`，如需修改可在 `src/app/app_controller.cpp` 的构造函数中调整。
- PC 版传输到本机 `received/`；板端通过 TCP 传输到服务器。
- 界面为中文，使用 FreeType 渲染 CJK 字体；字体随工程打包于 `assets/fonts/`，缺失时自动回退到系统字体。

## 板端交叉编译（i.MX6ULL / Buildroot）

目标板：100ask_IMX6ULL（Buildroot 2020.02，armv7l，1024x600 RGB LCD，goodix 触摸屏）。

前置条件：
- 主机已安装 ADB，且 `adb devices` 能看到板子。
- 主机存在交叉工具链：`/usr/local/arm/gcc-linaro-4.9.4-2017.01-x86_64_arm-linux-gnueabihf/`。
- 板端安装 `ffmpeg`（含 `alsa` 输入、`aac` 编码和 `mp4` muxer）与 GStreamer。

编译：

```bash
./scripts/build_board.sh
```

产物：`build_board/camera_lvgl`。

部署到板端并运行：

```bash
adb shell mkdir -p /root/camera_lvgl/assets/fonts
adb push build_board/camera_lvgl /root/camera_lvgl/camera_lvgl
adb push assets/fonts/DroidSansFallbackFull.ttf /root/camera_lvgl/assets/fonts/
adb shell 'chmod +x /root/camera_lvgl/camera_lvgl'
adb shell 'cd /root/camera_lvgl && setsid nohup ./camera_lvgl > /tmp/camera_lvgl.log 2>&1 < /dev/null &'
```

板端录像文件保存在 `/root/camera_lvgl/recordings/`，新文件为 `.mp4`，相册仍兼容旧的 `.avi`。点击相册右下角的 **传输到服务器** 后，板子会通过 TCP 把文件发送到 PC 的 `received/` 目录。

程序会优先从 `/proc/asound/cards` 选择 USB 麦克风（当前为 `plughw:1,0`），没有 USB 声卡时退回板载 WM8960。也可以固定音频设备：

```bash
adb shell 'echo plughw:1,0 > /root/camera_lvgl/audio_device.txt'
adb shell '/etc/init.d/S99camera restart'
```

录音问题可查看 `/tmp/camera_recording.log`。

相册中的录像使用板端 GStreamer（`playbin` + `fbdevsink` + `alsasink`）全屏播放，画面通过黑边适配 1024×600，AAC 音频输出到麦克风所属的 USB 声卡。双击影片后 LVGL 会暂停刷新，播放结束或失败后自动恢复相册画面；播放日志位于 `/tmp/camera_playback.log`。

板端内核默认启用只读参数 `consoleblank=600`。开机守护脚本 `camera_supervisor.sh` 会每秒解除 framebuffer blank，因此相册空闲、录像播放结束或主程序异常退出时都不会永久熄屏；相机进程异常退出后也会在 1～2 秒内自动重启。主程序自身仍每 5 秒发送一次 `FBIOBLANK(UNBLANK)` 作为第二层保护。

PC 端接收服务：

```bash
./tools/pc_receiver.py
```

默认监听 `0.0.0.0:9000`，接收目录为项目根目录 `received/`。板端默认传输目标为 `10.151.36.173:9000`。服务器 IP 变化时无需重新编译，可直接在板端修改：

```bash
adb shell 'echo 10.151.36.173 > /root/camera_lvgl/server_ip.txt'
adb shell '/etc/init.d/S99camera restart'
```

也可以通过环境变量 `CAMERA_SERVER_IP` 覆盖配置文件。传输前请确认板端已有 IPv4 地址并且可以 ping 通服务器。

## 浏览器网络实时预览

PC 端启动预览网关（保持终端运行）：

```bash
python3 tools/preview_server.py
```

浏览器打开 `http://<PC-IP>:8080`，本机也可打开 `http://127.0.0.1:8080`。然后在板端：

1. 点击 **开始预览**（或直接点击 **网络预览**，程序会自动启动摄像头）；
2. 点击 **网络预览**，按钮变为红色“直播中”；
3. 网页会显示实时画面、FPS、视频/音频码率和采样率；
4. 点击网页中的 **开启声音**，浏览器开始播放麦克风直播声音。

浏览器禁止网页在没有用户操作时自动播放声音，所以首次必须点击一次 **开启声音**。网络预览和录像可以同时开启，JPEG 帧只编码一次，麦克风也只采集一次，二者共用。再次点击板端 **网络预览** 可停止推流；PC 网关重启后板端会自动重连。

默认端口与协议：

- 板端视频 -> PC：`9002/TCP`，自定义帧头为 `>4sBBHIQHHHHI`（32 字节），帧类型和像素格式均为 JPEG；
- 板端声音 -> PC：`9003/TCP`，同尺寸 32 字节帧头，魔数为 `CAMA`，负载为 16 kHz 单声道 S16LE PCM；
- PC -> 浏览器：`8080/HTTP`，`/stream.mjpg` 为实时画面，`/stream.pcm` 为实时声音，`/api/status` 为音视频链路状态 JSON；
- 板端目标 IP 与录像传输共用 `/root/camera_lvgl/server_ip.txt`，网络变化时只需修改该文件并重启程序。

如果只看网页状态或排查连接问题：

```bash
curl http://127.0.0.1:8080/api/status
```

## 板端开机自启动

板端 init 脚本位于 `/etc/init.d/S99camera`，显示及进程守护脚本位于 `/root/camera_lvgl/camera_supervisor.sh`，仓库内分别备份为 `scripts/S99camera` 和 `scripts/camera_supervisor.sh`。

安装/更新：

```bash
adb push scripts/camera_supervisor.sh /root/camera_lvgl/camera_supervisor.sh
adb push scripts/S99camera /etc/init.d/S99camera
adb shell 'chmod +x /root/camera_lvgl/camera_supervisor.sh /etc/init.d/S99camera'
```

禁用原来的 LED/温湿度程序：

```bash
adb shell 'mv /etc/init.d/S99myqt /etc/init.d/K99myqt'
adb shell 'kill $(pidof myled) 2>/dev/null || true'
```

手动启动/停止相机程序：

```bash
adb shell '/etc/init.d/S99camera start'
adb shell '/etc/init.d/S99camera stop'
adb shell '/etc/init.d/S99camera restart'
```
