#!/usr/bin/env python3
"""PC 端网络预览网关。

板端通过 TCP 9002 推送带 32 字节帧头的 JPEG，本程序负责：
1. 校验并接收板端帧流；
2. 在 HTTP 8080 提供监控网页；
3. 将最新 JPEG 转成 multipart/x-mixed-replace，供浏览器实时显示。

只使用 Python 标准库，PC 无需安装额外依赖。
"""
from __future__ import annotations

import argparse
import json
import socket
import socketserver
import struct
import threading
import time
from collections import deque
from dataclasses import dataclass
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Optional, Tuple
from urllib.parse import urlsplit

BOARD_MAGIC = b"CAML"
FRAME_HEADER = struct.Struct(">4sBBHIQHHHHI")
FRAME_HEADER_SIZE = FRAME_HEADER.size
FRAME_TYPE_JPEG = 1
PIXEL_FORMAT_JPEG = 1
MAX_FRAME_SIZE = 8 * 1024 * 1024
STATS_WINDOW_SECONDS = 2.0
WEB_ROOT = Path(__file__).resolve().parent / "preview_web"


@dataclass(frozen=True)
class Frame:
    sequence: int
    timestamp_ms: int
    width: int
    height: int
    received_at: float
    jpeg: bytes


class FrameHub:
    """保存最新帧，并让多个 HTTP 客户端等待新帧。"""

    def __init__(self) -> None:
        self._condition = threading.Condition()
        self._latest: Optional[Frame] = None
        self._sequence = 0
        self._frames_received = 0
        self._stats: deque[Tuple[float, int]] = deque()
        self._source_count = 0
        self._source = ""
        self._viewers = 0
        self._stopping = False

    def publish(self, timestamp_ms: int, width: int, height: int, jpeg: bytes) -> None:
        now = time.monotonic()
        with self._condition:
            self._sequence += 1
            self._frames_received += 1
            self._latest = Frame(
                sequence=self._sequence,
                timestamp_ms=timestamp_ms,
                width=width,
                height=height,
                received_at=now,
                jpeg=jpeg,
            )
            self._stats.append((now, len(jpeg)))
            cutoff = now - STATS_WINDOW_SECONDS
            while self._stats and self._stats[0][0] < cutoff:
                self._stats.popleft()
            self._condition.notify_all()

    def set_source_connected(self, connected: bool, address: str = "") -> None:
        with self._condition:
            if connected:
                self._source_count += 1
                self._source = address
            else:
                self._source_count = max(0, self._source_count - 1)
                if self._source_count == 0:
                    self._source = ""
            self._condition.notify_all()

    def add_viewer(self, delta: int) -> None:
        with self._condition:
            self._viewers = max(0, self._viewers + delta)

    def wait_for_frame(self, after_sequence: int, timeout: float) -> Optional[Frame]:
        deadline = time.monotonic() + timeout
        with self._condition:
            while not self._stopping:
                if self._latest and self._latest.sequence != after_sequence:
                    return self._latest
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    return None
                self._condition.wait(remaining)
            return None

    def stop(self) -> None:
        with self._condition:
            self._stopping = True
            self._condition.notify_all()

    def snapshot(self) -> dict:
        now = time.monotonic()
        with self._condition:
            latest = self._latest
            source_connected = self._source_count > 0
            source = self._source
            viewers = self._viewers
            frames_received = self._frames_received
            stats = list(self._stats)

        streaming = bool(latest and now - latest.received_at < STATS_WINDOW_SECONDS)
        fps = 0.0
        bitrate_kbps = 0.0
        if len(stats) >= 2:
            elapsed = stats[-1][0] - stats[0][0]
            if elapsed > 0.05:
                fps = (len(stats) - 1) / elapsed
                bitrate_kbps = sum(size for _, size in stats) * 8.0 / elapsed / 1000.0

        return {
            "ok": True,
            "board_connected": source_connected,
            "streaming": streaming,
            "source": source or None,
            "width": latest.width if latest else 0,
            "height": latest.height if latest else 0,
            "fps": round(fps, 1),
            "bitrate_kbps": round(bitrate_kbps, 1),
            "frames_received": frames_received,
            "frame_age_ms": round((now - latest.received_at) * 1000) if latest else None,
            "viewers": viewers,
        }


class ReusableTCPServer(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True
    request_queue_size = 16


class ReusableHTTPServer(ThreadingHTTPServer):
    allow_reuse_address = True
    daemon_threads = True
    request_queue_size = 32


class BoardHandler(socketserver.BaseRequestHandler):
    def handle(self) -> None:
        conn: socket.socket = self.request
        peer = f"{self.client_address[0]}:{self.client_address[1]}"
        conn.setsockopt(socket.SOL_SOCKET, socket.SO_KEEPALIVE, 1)
        self.server.hub.set_source_connected(True, peer)  # type: ignore[attr-defined]
        print(f"[board] connected {peer}", flush=True)
        try:
            while True:
                header = recv_exact(conn, FRAME_HEADER_SIZE)
                (magic, version, frame_type, _flags, _generation,
                 timestamp_ms, width, height, pixel_format, _reserved,
                 payload_size) = FRAME_HEADER.unpack(header)

                if magic != BOARD_MAGIC:
                    raise ValueError("invalid frame magic")
                if version != 1:
                    raise ValueError(f"unsupported protocol version {version}")
                if frame_type != FRAME_TYPE_JPEG or pixel_format != PIXEL_FORMAT_JPEG:
                    raise ValueError("only JPEG frames are supported")
                if width == 0 or height == 0 or width > 4096 or height > 4096:
                    raise ValueError(f"invalid frame dimensions {width}x{height}")
                if payload_size == 0 or payload_size > MAX_FRAME_SIZE:
                    raise ValueError(f"invalid frame size {payload_size}")

                jpeg = recv_exact(conn, payload_size)
                self.server.hub.publish(timestamp_ms, width, height, jpeg)  # type: ignore[attr-defined]
        except (ConnectionError, OSError, ValueError) as exc:
            print(f"[board] disconnected {peer}: {exc}", flush=True)
        finally:
            self.server.hub.set_source_connected(False)  # type: ignore[attr-defined]


def recv_exact(conn: socket.socket, size: int) -> bytes:
    chunks = bytearray()
    while len(chunks) < size:
        chunk = conn.recv(size - len(chunks))
        if not chunk:
            raise ConnectionError("peer closed connection")
        chunks.extend(chunk)
    return bytes(chunks)


class PreviewHTTPHandler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    server_version = "CameraPreview/1.0"

    @property
    def hub(self) -> FrameHub:
        return self.server.hub  # type: ignore[attr-defined,no-any-return]

    def do_GET(self) -> None:
        path = urlsplit(self.path).path
        if path == "/api/status":
            self.send_json(self.hub.snapshot())
            return
        if path == "/healthz":
            self.send_json({"ok": True})
            return
        if path == "/stream.mjpg":
            self.serve_mjpeg()
            return
        if path == "/":
            self.serve_static("index.html", "text/html; charset=utf-8")
            return
        if path == "/app.js":
            self.serve_static("app.js", "application/javascript; charset=utf-8")
            return
        if path == "/style.css":
            self.serve_static("style.css", "text/css; charset=utf-8")
            return
        if path == "/favicon.ico":
            self.send_response(204)
            self.send_header("Content-Length", "0")
            self.end_headers()
            return
        self.send_error(404, "Not Found")

    def do_HEAD(self) -> None:
        path = urlsplit(self.path).path
        if path == "/":
            path = "/index.html"
        content_types = {
            "/index.html": "text/html; charset=utf-8",
            "/app.js": "application/javascript; charset=utf-8",
            "/style.css": "text/css; charset=utf-8",
        }
        if path not in content_types:
            self.send_error(404, "Not Found")
            return
        file_path = WEB_ROOT / path.lstrip("/")
        try:
            size = file_path.stat().st_size
        except OSError:
            self.send_error(404, "Not Found")
            return
        self.send_response(200)
        self.send_header("Content-Type", content_types[path])
        self.send_header("Content-Length", str(size))
        self.send_header("Cache-Control", "no-cache")
        self.end_headers()

    def serve_static(self, name: str, content_type: str) -> None:
        file_path = WEB_ROOT / name
        try:
            payload = file_path.read_bytes()
        except OSError:
            self.send_error(404, "Not Found")
            return
        self.send_response(200)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(payload)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(payload)

    def send_json(self, payload: dict) -> None:
        data = json.dumps(payload, ensure_ascii=False, separators=(",", ":")).encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(data)

    def serve_mjpeg(self) -> None:
        self.send_response(200)
        self.send_header("Content-Type", "multipart/x-mixed-replace; boundary=frame")
        self.send_header("Cache-Control", "no-cache, no-store, must-revalidate")
        self.send_header("Pragma", "no-cache")
        self.send_header("Connection", "close")
        self.end_headers()
        self.close_connection = True
        self.hub.add_viewer(1)
        last_sequence = -1
        try:
            while True:
                frame = self.hub.wait_for_frame(last_sequence, timeout=5.0)
                if frame is None:
                    continue
                self.wfile.write(b"--frame\r\n")
                self.wfile.write(b"Content-Type: image/jpeg\r\n")
                self.wfile.write(f"Content-Length: {len(frame.jpeg)}\r\n\r\n".encode("ascii"))
                self.wfile.write(frame.jpeg)
                self.wfile.write(b"\r\n")
                self.wfile.flush()
                last_sequence = frame.sequence
        except (BrokenPipeError, ConnectionResetError, OSError):
            pass
        finally:
            self.hub.add_viewer(-1)

    def log_message(self, fmt: str, *args: object) -> None:
        if args and str(args[0]).startswith("GET /stream.mjpg"):
            return
        print(f"[http] {self.address_string()} {fmt % args}", flush=True)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="摄像头网络实时预览 PC 网关")
    parser.add_argument("--board-host", default="0.0.0.0", help="板端帧流监听地址")
    parser.add_argument("--board-port", type=int, default=9002, help="板端帧流监听端口")
    parser.add_argument("--http-host", default="0.0.0.0", help="网页监听地址")
    parser.add_argument("--http-port", type=int, default=8080, help="网页监听端口")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    if not WEB_ROOT.is_dir():
        print(f"missing web directory: {WEB_ROOT}", flush=True)
        return 1

    hub = FrameHub()
    board_server = ReusableTCPServer((args.board_host, args.board_port), BoardHandler)
    board_server.hub = hub  # type: ignore[attr-defined]
    http_server = ReusableHTTPServer((args.http_host, args.http_port), PreviewHTTPHandler)
    http_server.hub = hub  # type: ignore[attr-defined]

    print(f"[board] listening {args.board_host}:{args.board_port}", flush=True)
    print(f"[web] http://<PC-IP>:{args.http_port}", flush=True)

    http_thread = threading.Thread(target=http_server.serve_forever, name="http-server", daemon=True)
    http_thread.start()
    try:
        board_server.serve_forever()
    except KeyboardInterrupt:
        print("\nstopping", flush=True)
    finally:
        hub.stop()
        board_server.shutdown()
        http_server.shutdown()
        board_server.server_close()
        http_server.server_close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
