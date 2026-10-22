#!/usr/bin/env python3
"""PC 端接收服务：接收板端通过 TCP 发来的录像文件，保存到项目 received/ 目录。

协议头（网络字节序，共 16 字节）：
    4s   magic  固定为 b"CAMX"
    I    name_len  文件名长度
    Q    file_size 文件大小
之后依次发送：文件名原始字节、文件内容。
"""
import os
import socket
import struct
import sys

HOST = "0.0.0.0"
PORT = 9000
ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
DEST = os.path.join(ROOT, "received")


def recv_exact(conn: socket.socket, n: int) -> bytes:
    buf = b""
    while len(buf) < n:
        chunk = conn.recv(n - len(buf))
        if not chunk:
            raise ConnectionError("connection closed")
        buf += chunk
    return buf


def handle(conn: socket.socket, addr):
    try:
        header = recv_exact(conn, 16)
        magic, name_len, file_size = struct.unpack(">4sIQ", header)
        if magic != b"CAMX":
            raise ValueError("bad magic")

        raw_name = recv_exact(conn, name_len)
        name = raw_name.decode("utf-8", "replace")
        safe_name = os.path.basename(name) or "received.mp4"
        dest = os.path.join(DEST, safe_name)

        os.makedirs(DEST, exist_ok=True)
        remaining = file_size
        with open(dest, "wb") as f:
            while remaining > 0:
                chunk = conn.recv(min(1024 * 1024, remaining))
                if not chunk:
                    raise ConnectionError("connection closed during payload")
                f.write(chunk)
                remaining -= len(chunk)

        print(f"[recv] {addr[0]}:{addr[1]} -> {dest} ({file_size} bytes)", flush=True)
        conn.sendall(b"1")
    except Exception as exc:
        print(f"[error] {addr[0]}:{addr[1]} {exc}", flush=True)
        try:
            conn.sendall(b"0")
        except OSError:
            pass
    finally:
        conn.close()


def main():
    os.makedirs(DEST, exist_ok=True)
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as srv:
        srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        srv.bind((HOST, PORT))
        srv.listen(4)
        print(f"[listen] {HOST}:{PORT} -> {DEST}", flush=True)
        while True:
            conn, addr = srv.accept()
            handle(conn, addr)


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print("stopped", file=sys.stderr)
        raise SystemExit(0)
