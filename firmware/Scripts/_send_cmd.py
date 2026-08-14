#!/usr/bin/env python3
"""临时诊断：向设备发送 CLI 命令并捕获输出，直接写文件。用后即删。"""
import serial
import time

PORT = 'COM3'
BAUD = 115200
OUT = 'Scripts/_send_cmd_out.txt'

def send_cmd(ser, cmd, wait_s=10):
    ser.reset_input_buffer()
    ser.write((cmd + '\r\n').encode())
    buf = b''
    end = time.time() + wait_s
    while time.time() < end:
        n = ser.in_waiting
        if n:
            buf += ser.read(n)
        else:
            time.sleep(0.05)
    return buf.decode('utf-8', errors='ignore')

lines = []
ser = serial.Serial(PORT, BAUD, timeout=0.2)
time.sleep(0.5)
ser.reset_input_buffer()

for c in ['tcp_test 192.168.11.100 8000 256',
          'udp_test 192.168.11.100 8001 256']:
    lines.append(f"===== CMD: {c} =====")
    out = send_cmd(ser, c)
    lines.append(repr(out[-800:]))
    lines.append("")

ser.close()
with open(OUT, 'w', encoding='utf-8') as f:
    f.write('\n'.join(lines))
