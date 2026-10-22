#!/bin/sh

APP="/root/camera_lvgl/camera_lvgl"
APP_DIR="/root/camera_lvgl"
APP_PIDFILE="/var/run/camera_lvgl.pid"
LOG_FILE="/tmp/camera_lvgl.log"
FB_BLANK="/sys/class/graphics/fb0/blank"

cd "$APP_DIR" || exit 1

while :; do
    # 该板 consoleblank 为只读参数（默认 600 秒），周期性解除 framebuffer blank。
    printf '0' > "$FB_BLANK" 2>/dev/null || true

    if ! pidof camera_lvgl >/dev/null 2>&1; then
        "$APP" >>"$LOG_FILE" 2>&1 </dev/null &
        echo $! > "$APP_PIDFILE"
        sleep 2
    fi

    sleep 1
done
