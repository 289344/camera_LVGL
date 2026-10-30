#!/usr/bin/env python3
"""ADB 显示回归：先播放有运动的录像，再确认 framebuffer 持续变化。

只读屏幕中央的四行，不保存录像画面。变化次数是采样结果，不是实际 FPS。
适用于项目的 1024x600、32bpp framebuffer；仅需 Python 标准库与 adb。
"""
import argparse
import base64
import hashlib
import subprocess
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--seconds", type=float, default=6)
    parser.add_argument("--min-updates", type=int, default=10)
    args = parser.parse_args()
    if args.seconds <= 0 or args.min_updates < 1:
        parser.error("seconds 和 min-updates 必须大于零")

    command = "dd if=/dev/fb0 bs=4096 skip=298 count=4 2>/dev/null | base64"
    started = time.monotonic()
    previous = None
    updates = 0
    samples = 0
    while time.monotonic() - started < args.seconds:
        result = subprocess.run(
            ["adb", "shell", command], check=True, capture_output=True, timeout=5
        )
        raw = base64.b64decode(result.stdout)
        if len(raw) != 4 * 4096:
            raise RuntimeError("无法读取 1024x600/32bpp framebuffer")
        # 原尺寸视频居中到 x=192..831，忽略黑边和控制台光标。
        pixels = b"".join(raw[y * 4096 + 192 * 4:y * 4096 + 832 * 4] for y in range(4))
        current = hashlib.sha256(pixels).digest()
        if previous is not None and current != previous:
            updates += 1
        previous = current
        samples += 1
        time.sleep(0.15)

    print(f"framebuffer: {samples} samples, {updates} observed updates")
    if updates < args.min_updates:
        raise SystemExit("FAIL: 画面变化不足；请使用有运动的录像检查是否停在首帧")
    print("PASS: 连续画面变化，未停在首帧（此结果不代表实际 FPS）")


if __name__ == "__main__":
    main()
