#!/usr/bin/env python3
"""网络预览网关的端到端冒烟测试，仅使用 Python 标准库。"""
import http.client
import json
import os
import socket
import struct
import subprocess
import sys
import time
import unittest
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SERVER = ROOT / "tools" / "preview_server.py"
HEADER = struct.Struct(">4sBBHIQHHHHI")


def free_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


class PreviewServerTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.http_port = free_port()
        cls.board_port = free_port()
        cls.process = subprocess.Popen(
            [sys.executable, str(SERVER),
             "--http-host", "127.0.0.1", "--http-port", str(cls.http_port),
             "--board-host", "127.0.0.1", "--board-port", str(cls.board_port)],
            cwd=ROOT,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            try:
                with urllib.request.urlopen(cls.url("/healthz"), timeout=0.3) as response:
                    if response.status == 200:
                        return
            except OSError:
                time.sleep(0.05)
        raise RuntimeError("preview server did not start")

    @classmethod
    def tearDownClass(cls) -> None:
        cls.process.terminate()
        try:
            cls.process.wait(timeout=3)
        except subprocess.TimeoutExpired:
            cls.process.kill()
            cls.process.wait(timeout=3)

    @classmethod
    def url(cls, path: str) -> str:
        return f"http://127.0.0.1:{cls.http_port}{path}"

    def test_frame_ingest_and_browser_stream(self) -> None:
        payload = b"\xff\xd8preview-smoke-test\xff\xd9"
        with socket.create_connection(("127.0.0.1", self.board_port), timeout=2) as board:
            for sequence in (1, 2):
                header = HEADER.pack(
                    b"CAML", 1, 1, 0, sequence, int(time.time() * 1000),
                    320, 240, 1, 0, len(payload),
                )
                board.sendall(header + payload)

            deadline = time.monotonic() + 2
            status = {}
            while time.monotonic() < deadline:
                with urllib.request.urlopen(self.url("/api/status"), timeout=1) as response:
                    status = json.load(response)
                if status.get("frames_received", 0) >= 2:
                    break
                time.sleep(0.05)

            self.assertTrue(status["board_connected"])
            self.assertTrue(status["streaming"])
            self.assertEqual((status["width"], status["height"]), (320, 240))

            conn = http.client.HTTPConnection("127.0.0.1", self.http_port, timeout=2)
            conn.request("GET", "/stream.mjpg")
            response = conn.getresponse()
            self.assertEqual(response.status, 200)
            self.assertTrue(response.getheader("Content-Type").startswith("multipart/x-mixed-replace"))
            data = response.read(len(b"--frame\r\nContent-Type: image/jpeg\r\nContent-Length: 27\r\n\r\n") + len(payload))
            conn.close()
            self.assertIn(b"--frame", data)
            self.assertIn(payload, data)

        deadline = time.monotonic() + 2
        while time.monotonic() < deadline:
            with urllib.request.urlopen(self.url("/api/status"), timeout=1) as response:
                status = json.load(response)
            if not status["board_connected"]:
                break
            time.sleep(0.05)
        self.assertFalse(status["board_connected"])


if __name__ == "__main__":
    unittest.main(verbosity=2)
