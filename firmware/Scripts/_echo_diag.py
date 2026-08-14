#!/usr/bin/env python3
"""临时诊断：TCP 8000 + UDP 8001 echo 服务，记录所有入站包/连接。
用于判断设备的 TCP/UDP 包是否真正到达 PC。用后即删。"""
import socket
import threading
import time

LOG = 'Scripts/_echo_diag.log'

def log(msg):
    line = f"[{time.time():.3f}] {msg}"
    print(line, flush=True)
    with open(LOG, 'a') as f:
        f.write(line + '\n')

def tcp_server():
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(('0.0.0.0', 8000))
    s.listen(8)
    log("TCP 8000 listening")
    while True:
        conn, addr = s.accept()
        log(f"TCP CONN from {addr}")
        try:
            data = conn.recv(2048)
            log(f"TCP RECV from {addr} len={len(data)}")
            if data:
                conn.sendall(data)
                log(f"TCP SENT back to {addr}")
        except Exception as e:
            log(f"TCP ERR {e}")
        finally:
            conn.close()

def udp_server():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(('0.0.0.0', 8001))
    log("UDP 8001 listening")
    while True:
        data, addr = s.recvfrom(2048)
        log(f"UDP RECV from {addr} len={len(data)}")
        s.sendto(data, addr)
        log(f"UDP SENT back to {addr}")

with open(LOG, 'w') as f:
    f.write("")
t = threading.Thread(target=tcp_server, daemon=True)
t.start()
u = threading.Thread(target=udp_server, daemon=True)
u.start()
log("diag echo servers running (Ctrl+C to stop)")
while True:
    time.sleep(3600)
