#!/usr/bin/env python3
"""PC 端网络音视频预览网关。

板端使用两条低依赖 TCP 链路：
1. 9002/TCP：32 字节帧头 + JPEG 画面；
2. 9003/TCP：32 字节帧头 + 16 kHz 单声道 S16LE PCM 声音。

本程序只使用 Python 标准库，在 8080/HTTP 向浏览器同时提供 MJPEG
画面和原始 PCM 音频流。音频对每个浏览器客户端使用有界队列，慢客户端
只会丢弃旧音频块，不会让延迟无限增长。
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
from typing import Deque, Optional, Tuple
from urllib.parse import urlsplit

VIDEO_MAGIC = b"CAML"
VIDEO_HEADER = struct.Struct(">4sBBHIQHHHHI")
VIDEO_HEADER_SIZE = VIDEO_HEADER.size
VIDEO_FRAME_TYPE_JPEG = 1
VIDEO_PIXEL_FORMAT_JPEG = 1
MAX_VIDEO_PAYLOAD = 8 * 1024 * 1024

AUDIO_MAGIC = b"CAMA"
AUDIO_HEADER = struct.Struct(">4sBBHIQHHHHI")
AUDIO_HEADER_SIZE = AUDIO_HEADER.size
AUDIO_FRAME_TYPE_PCM = 1
AUDIO_FORMAT_S16LE = 1
MAX_AUDIO_PAYLOAD = 64 * 1024
AUDIO_QUEUE_CHUNKS = 25  # 16 kHz 单声道时约 1 秒，超出后丢最旧块。
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
    """保存最新画面，并让多个 HTTP 客户端等待新帧。"""

    def __init__(self) -> None:
        self._condition = threading.Condition()
        self._latest: Optional[Frame] = None
        self._sequence = 0
        self._frames_received = 0
        self._stats: Deque[Tuple[float, int]] = deque()
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


class AudioHub:
    """把板端 PCM 分发给浏览器，并为每个慢客户端维持有界队列。"""

    def __init__(self) -> None:
        self._condition = threading.Condition()
        self._subscribers: list[Deque[bytes]] = []
        self._source_count = 0
        self._source = ""
        self._packets_received = 0
        self._latest_at = 0.0
        self._sample_rate = 0
        self._channels = 0
        self._stats: Deque[Tuple[float, int]] = deque()
        self._dropped_chunks = 0
        self._stopping = False

    def publish(self, sample_rate: int, channels: int, pcm: bytes) -> None:
        now = time.monotonic()
        with self._condition:
            self._packets_received += 1
            self._latest_at = now
            self._sample_rate = sample_rate
            self._channels = channels
            self._stats.append((now, len(pcm)))
            cutoff = now - STATS_WINDOW_SECONDS
            while self._stats and self._stats[0][0] < cutoff:
                self._stats.popleft()

            for queue in self._subscribers:
                if len(queue) >= AUDIO_QUEUE_CHUNKS:
                    queue.popleft()
                    self._dropped_chunks += 1
                queue.append(pcm)
            self._condition.notify_all()

    def subscribe(self) -> Deque[bytes]:
        queue: Deque[bytes] = deque()
        with self._condition:
            self._subscribers.append(queue)
        return queue

    def unsubscribe(self, queue: Deque[bytes]) -> None:
        with self._condition:
            try:
                self._subscribers.remove(queue)
            except ValueError:
                pass
            self._condition.notify_all()

    def wait_for_chunk(self, queue: Deque[bytes], timeout: float) -> Optional[bytes]:
        deadline = time.monotonic() + timeout
        with self._condition:
            while not self._stopping:
                if queue:
                    return queue.popleft()
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    return None
                self._condition.wait(remaining)
            return None

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

    def stop(self) -> None:
        with self._condition:
            self._stopping = True
            self._condition.notify_all()

    def snapshot(self) -> dict:
        now = time.monotonic()
        with self._condition:
            connected = self._source_count > 0
            source = self._source
            packets_received = self._packets_received
            latest_at = self._latest_at
            sample_rate = self._sample_rate
            channels = self._channels
            stats = list(self._stats)
            viewers = len(self._subscribers)
            dropped_chunks = self._dropped_chunks

        streaming = bool(latest_at and now - latest_at < STATS_WINDOW_SECONDS)
        bitrate_kbps = 0.0
        if len(stats) >= 2:
            elapsed = stats[-1][0] - stats[0][0]
            if elapsed > 0.05:
                bitrate_kbps = sum(size for _, size in stats) * 8.0 / elapsed / 1000.0

        return {
            "audio_connected": connected,
            "audio_streaming": streaming,
            "audio_source": source or None,
            "audio_sample_rate": sample_rate,
            "audio_channels": channels,
            "audio_format": "S16LE" if sample_rate else None,
            "audio_bitrate_kbps": round(bitrate_kbps, 1),
            "audio_packets_received": packets_received,
            "audio_packet_age_ms": round((now - latest_at) * 1000) if latest_at else None,
            "audio_viewers": viewers,
            "audio_dropped_chunks": dropped_chunks,
        }


class ReusableTCPServer(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True
    request_queue_size = 16


class ReusableHTTPServer(ThreadingHTTPServer):
    allow_reuse_address = True
    daemon_threads = True
    request_queue_size = 32


class VideoBoardHandler(socketserver.BaseRequestHandler):
    def handle(self) -> None:
        conn: socket.socket = self.request
        peer = f"{self.client_address[0]}:{self.client_address[1]}"
        conn.setsockopt(socket.SOL_SOCKET, socket.SO_KEEPALIVE, 1)
        self.server.hub.set_source_connected(True, peer)  # type: ignore[attr-defined]
        print(f"[video] connected {peer}", flush=True)
        try:
            while True:
                header = recv_exact(conn, VIDEO_HEADER_SIZE)
                (magic, version, frame_type, _flags, _generation,
                 timestamp_ms, width, height, pixel_format, _reserved,
                 payload_size) = VIDEO_HEADER.unpack(header)

                if magic != VIDEO_MAGIC:
                    raise ValueError("invalid video magic")
                if version != 1:
                    raise ValueError(f"unsupported video protocol version {version}")
                if frame_type != VIDEO_FRAME_TYPE_JPEG or pixel_format != VIDEO_PIXEL_FORMAT_JPEG:
                    raise ValueError("only JPEG video frames are supported")
                if width == 0 or height == 0 or width > 4096 or height > 4096:
                    raise ValueError(f"invalid video dimensions {width}x{height}")
                if payload_size == 0 or payload_size > MAX_VIDEO_PAYLOAD:
                    raise ValueError(f"invalid video size {payload_size}")

                jpeg = recv_exact(conn, payload_size)
                self.server.hub.publish(timestamp_ms, width, height, jpeg)  # type: ignore[attr-defined]
        except (ConnectionError, OSError, ValueError) as exc:
            print(f"[video] disconnected {peer}: {exc}", flush=True)
        finally:
            self.server.hub.set_source_connected(False)  # type: ignore[attr-defined]


class AudioBoardHandler(socketserver.BaseRequestHandler):
    def handle(self) -> None:
        conn: socket.socket = self.request
        peer = f"{self.client_address[0]}:{self.client_address[1]}"
        conn.setsockopt(socket.SOL_SOCKET, socket.SO_KEEPALIVE, 1)
        self.server.audio_hub.set_source_connected(True, peer)  # type: ignore[attr-defined]
        print(f"[audio] connected {peer}", flush=True)
        try:
            while True:
                header = recv_exact(conn, AUDIO_HEADER_SIZE)
                (magic, version, frame_type, _flags, _generation,
                 _timestamp_ms, sample_rate, channels, sample_format, _reserved,
                 payload_size) = AUDIO_HEADER.unpack(header)

                if magic != AUDIO_MAGIC:
                    raise ValueError("invalid audio magic")
                if version != 1:
                    raise ValueError(f"unsupported audio protocol version {version}")
                if frame_type != AUDIO_FRAME_TYPE_PCM or sample_format != AUDIO_FORMAT_S16LE:
                    raise ValueError("only S16LE PCM audio is supported")
                if sample_rate < 8000 or sample_rate > 48000:
                    raise ValueError(f"invalid sample rate {sample_rate}")
                if channels not in (1, 2):
                    raise ValueError(f"invalid channel count {channels}")
                if payload_size == 0 or payload_size > MAX_AUDIO_PAYLOAD:
                    raise ValueError(f"invalid audio size {payload_size}")
                if payload_size % (2 * channels) != 0:
                    raise ValueError("audio payload is not frame aligned")

                pcm = recv_exact(conn, payload_size)
                self.server.audio_hub.publish(sample_rate, channels, pcm)  # type: ignore[attr-defined]
        except (ConnectionError, OSError, ValueError) as exc:
            print(f"[audio] disconnected {peer}: {exc}", flush=True)
        finally:
            self.server.audio_hub.set_source_connected(False)  # type: ignore[attr-defined]


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
    server_version = "CameraPreview/1.1"

    @property
    def hub(self) -> FrameHub:
        return self.server.hub  # type: ignore[attr-defined,no-any-return]

    @property
    def audio_hub(self) -> AudioHub:
        return self.server.audio_hub  # type: ignore[attr-defined,no-any-return]

    def do_GET(self) -> None:
        path = urlsplit(self.path).path
        if path == "/api/status":
            payload = self.hub.snapshot()
            payload.update(self.audio_hub.snapshot())
            self.send_json(payload)
            return
        if path == "/healthz":
            self.send_json({"ok": True})
            return
        if path == "/stream.mjpg":
            self.serve_mjpeg()
            return
        if path == "/stream.pcm":
            self.serve_pcm()
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

    def serve_pcm(self) -> None:
        status = self.audio_hub.snapshot()
        sample_rate = status["audio_sample_rate"] or 16000
        channels = status["audio_channels"] or 1
        self.send_response(200)
        self.send_header("Content-Type", f"audio/L16;rate={sample_rate};channels={channels}")
        self.send_header("Cache-Control", "no-cache, no-store, must-revalidate")
        self.send_header("Pragma", "no-cache")
        self.send_header("X-Audio-Sample-Rate", str(sample_rate))
        self.send_header("X-Audio-Channels", str(channels))
        self.send_header("X-Audio-Format", "S16LE")
        self.send_header("Connection", "close")
        self.end_headers()
        self.close_connection = True
        queue = self.audio_hub.subscribe()
        try:
            while True:
                pcm = self.audio_hub.wait_for_chunk(queue, timeout=5.0)
                if pcm is None:
                    continue
                self.wfile.write(pcm)
                self.wfile.flush()
        except (BrokenPipeError, ConnectionResetError, OSError):
            pass
        finally:
            self.audio_hub.unsubscribe(queue)

    def log_message(self, fmt: str, *args: object) -> None:
        if args and (str(args[0]).startswith("GET /stream.mjpg") or
                     str(args[0]).startswith("GET /stream.pcm") or
                     str(args[0]).startswith("GET /api/status")):
            return
        print(f"[http] {self.address_string()} {fmt % args}", flush=True)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="摄像头网络音视频实时预览 PC 网关")
    parser.add_argument("--board-host", default="0.0.0.0", help="板端视频流监听地址")
    parser.add_argument("--board-port", type=int, default=9002, help="板端视频流监听端口")
    parser.add_argument("--audio-host", default="0.0.0.0", help="板端音频流监听地址")
    parser.add_argument("--audio-port", type=int, default=9003, help="板端音频流监听端口")
    parser.add_argument("--http-host", default="0.0.0.0", help="网页监听地址")
    parser.add_argument("--http-port", type=int, default=8080, help="网页监听端口")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    if not WEB_ROOT.is_dir():
        print(f"missing web directory: {WEB_ROOT}", flush=True)
        return 1

    video_hub = FrameHub()
    audio_hub = AudioHub()
    video_server = ReusableTCPServer((args.board_host, args.board_port), VideoBoardHandler)
    video_server.hub = video_hub  # type: ignore[attr-defined]
    audio_server = ReusableTCPServer((args.audio_host, args.audio_port), AudioBoardHandler)
    audio_server.audio_hub = audio_hub  # type: ignore[attr-defined]
    http_server = ReusableHTTPServer((args.http_host, args.http_port), PreviewHTTPHandler)
    http_server.hub = video_hub  # type: ignore[attr-defined]
    http_server.audio_hub = audio_hub  # type: ignore[attr-defined]

    servers = [video_server, audio_server, http_server]
    threads = [
        threading.Thread(target=video_server.serve_forever, name="video-server", daemon=True),
        threading.Thread(target=audio_server.serve_forever, name="audio-server", daemon=True),
        threading.Thread(target=http_server.serve_forever, name="http-server", daemon=True),
    ]
    for thread in threads:
        thread.start()

    print(f"[video] listening {args.board_host}:{args.board_port}", flush=True)
    print(f"[audio] listening {args.audio_host}:{args.audio_port}", flush=True)
    print(f"[web] http://<PC-IP>:{args.http_port}", flush=True)

    try:
        while True:
            time.sleep(1.0)
    except KeyboardInterrupt:
        print("\nstopping", flush=True)
    finally:
        video_hub.stop()
        audio_hub.stop()
        for server in servers:
            server.shutdown()
        for server in servers:
            server.server_close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
