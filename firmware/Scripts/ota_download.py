#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
ota_download.py - 通过串口 CLI 协议下发固件到设备（STM32 A/B OTA）

支持格式：
  .bin  : 原始二进制（目标分区相对偏移从 0 开始）
  .hex  : Intel HEX（自动解析；地址必须落在 A 区或 B 区起点）
  .elf  : 自动调用 arm-none-eabi-objcopy 转 .bin（需工具链在 PATH）

用法：
  python ota_download.py -p COM3 -f app.bin
  python ota_download.py -p COM3 -f app.hex -v 3 -a
  python ota_download.py -p COM3 -f app.elf -b 921600

说明：
  - CRC32 使用 zlib.crc32（IEEE 802.3），与固件 ota_crc32 一致
  - 下载流程：ota begin -> 分块 ota write -> ota end(校验) -> [ota activate]
  - 固件大小上限 384KB（App B 区容量，取 A/B 分区较小值）
"""

import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
import zlib

try:
    import serial
except ImportError:
    print("[ERROR] 缺少 pyserial，请先安装: pip install pyserial")
    sys.exit(1)

# 与固件 ota_flash_port.h 保持一致的常量
OTA_APP_A_BASE = 0x08010000
OTA_APP_B_BASE = 0x08080000
OTA_FW_MAX_SIZE = 384 * 1024          # 目标分区容量（App B）
OTA_BLOCK_SIZE = 112                  # 每块字节数（CLI输入缓冲256，hex上限约120）
OTA_BEGIN_WAIT_S = 40                 # begin 含擦除（3~4个扇区，最慢约4s/扇区）
OTA_WRITE_WAIT_S = 5
OTA_END_WAIT_S = 30                   # end 含读回+CRC32校验
OTA_ACTIVATE_WAIT_S = 5


def parse_intel_hex(path):
    """解析 Intel HEX 文件，返回 (起始地址, 连续数据bytes)。"""
    min_addr = None
    max_addr = None
    ext_base = 0
    buf = {}

    with open(path, 'r') as f:
        for line_no, line in enumerate(f, 1):
            line = line.strip()
            if not line:
                continue
            if not line.startswith(':'):
                raise ValueError(f"[HEX] 第{line_no}行不是 ':' 开头")
            raw = bytes.fromhex(line[1:])
            length = raw[0]
            addr = int.from_bytes(raw[1:3], 'big')
            rtype = raw[3]
            data = raw[4:4 + length]
            chk = raw[4 + length]
            if (sum(raw[:-1]) + chk) & 0xFF != 0:
                raise ValueError(f"[HEX] 第{line_no}行校验和错误")

            if rtype == 0x00:                     # 数据记录
                full = ext_base + addr
                for i, b in enumerate(data):
                    buf[full + i] = b
                if min_addr is None or full < min_addr:
                    min_addr = full
                if max_addr is None or (full + length) > max_addr:
                    max_addr = full + length
            elif rtype == 0x01:                   # EOF
                break
            elif rtype == 0x04:                   # 扩展线性地址
                ext_base = int.from_bytes(data[:2], 'big') << 16
            elif rtype == 0x02:                   # 扩展段地址
                ext_base = int.from_bytes(data[:2], 'big') << 4
            # 其他记录类型忽略

    if min_addr is None:
        raise ValueError("[HEX] 文件中没有数据记录")
    out = bytes(buf.get(a, 0xFF) for a in range(min_addr, max_addr))
    return min_addr, out


def elf_to_bin(path):
    """调用 arm-none-eabi-objcopy 将 ELF 转二进制。"""
    objcopy = shutil.which('arm-none-eabi-objcopy')
    if objcopy is None:
        raise RuntimeError(
            "[ELF] 未找到 arm-none-eabi-objcopy，请将其加入 PATH，"
            "或先用 objcopy 手动转换: arm-none-eabi-objcopy -O binary app.elf app.bin")
    fd, tmp = tempfile.mkstemp(suffix='.bin')
    os.close(fd)
    try:
        subprocess.run([objcopy, '-O', 'binary', path, tmp], check=True)
        with open(tmp, 'rb') as f:
            return f.read()
    finally:
        os.unlink(tmp)


def resolve_fw(path):
    """加载固件文件，返回 (二进制数据, 起始地址)。"""
    ext = os.path.splitext(path)[1].lower()
    if ext == '.bin':
        with open(path, 'rb') as f:
            data = f.read()
        base = 0
    elif ext == '.hex':
        base, data = parse_intel_hex(path)
    elif ext == '.elf':
        data = elf_to_bin(path)
        base = 0
    else:
        raise ValueError(f"不支持的格式 '{ext}'（支持 .bin/.hex/.elf）")

    if len(data) > OTA_FW_MAX_SIZE:
        raise ValueError(f"固件过大: {len(data)} 字节 > 上限 {OTA_FW_MAX_SIZE} 字节")

    # hex 带地址信息：必须落在 A/B 分区起点（App 按分区地址链接）
    if base != 0 and base not in (OTA_APP_A_BASE, OTA_APP_B_BASE):
        raise ValueError(
            f"[HEX] 起始地址 0x{base:08X} 不是分区起点\n"
            f"       A区=0x{OTA_APP_A_BASE:08X}  B区=0x{OTA_APP_B_BASE:08X}\n"
            f"       请将 App 链接脚本 FLASH ORIGIN 改为分区地址后重新编译")

    return data, base


class OtaDownloader:
    def __init__(self, port, baud):
        self.port = port
        self.baud = baud
        self.ser = None

    def connect(self):
        self.ser = serial.Serial(self.port, self.baud, timeout=0.2)
        time.sleep(0.3)
        self.ser.reset_input_buffer()

    def disconnect(self):
        if self.ser is not None:
            self.ser.close()
            self.ser = None

    def read_all(self, duration):
        end = time.time() + duration
        buf = b''
        while time.time() < end:
            n = self.ser.in_waiting
            if n:
                buf += self.ser.read(n)
            else:
                time.sleep(0.05)
        return buf.decode('utf-8', 'ignore')

    def cmd(self, text, wait=5.0):
        """发送一行命令并收集响应。"""
        self.ser.write(text.encode() + b'\r\n')
        return self.read_all(wait)

    def wait_prompt(self, timeout=10):
        """等待 CLI 提示符出现。"""
        end = time.time() + timeout
        buf = b''
        while time.time() < end:
            c = self.ser.read(1)
            if c:
                buf += c
                if b'>' in buf:
                    return True
        return False

    def run(self, data, version, auto_activate):
        print(f"[INFO] Port={self.port}, Baud={self.baud}")
        self.connect()

        # 唤醒并确认 CLI 就绪
        self.ser.write(b'\r\n')
        if not self.wait_prompt():
            raise RuntimeError("未检测到 CLI 提示符，请确认固件已运行")
        print("[OK] CLI ready")

        size = len(data)
        crc = zlib.crc32(data) & 0xFFFFFFFF
        print(f"[INFO] Firmware: {size} bytes, CRC32=0x{crc:08X}, version={version}")

        # 1. begin（擦除非活跃区）
        out = self.cmd(f'ota begin {size} {crc:x} {version}', wait=OTA_BEGIN_WAIT_S)
        if 'OK' not in out:
            raise RuntimeError(f"ota begin 失败: {self._brief(out)}")
        print("[OK] begin (target slot erased)")

        # 2. 分块写入（每块最多重试2次）
        last_pct = -1
        off = 0
        while off < size:
            chunk = data[off:off + OTA_BLOCK_SIZE]
            for attempt in range(3):
                out = self.cmd(f'ota write {off} {chunk.hex()}', wait=OTA_WRITE_WAIT_S)
                if 'OK' in out:
                    break
            else:
                raise RuntimeError(f"ota write @0x{off:X} 失败: {self._brief(out)}")

            off += len(chunk)
            pct = off * 100 // size
            if pct >= last_pct + 10 or off >= size:
                print(f"[..] {off}/{size} bytes ({pct}%)")
                last_pct = pct
        print("[OK] all blocks written")

        # 3. end（读回 + CRC32 校验）
        out = self.cmd('ota end', wait=OTA_END_WAIT_S)
        if 'OK' not in out:
            raise RuntimeError(f"ota end 校验失败: {self._brief(out)}")
        print("[OK] CRC32 verify passed")

        # 4. activate（可选）
        if auto_activate:
            out = self.cmd('ota activate', wait=OTA_ACTIVATE_WAIT_S)
            if 'OK' not in out:
                raise RuntimeError(f"ota activate 失败: {self._brief(out)}")
            print("[OK] activate (reboot to apply)")
        else:
            print("[INFO] 未自动激活；确认无误后执行: ota activate")

        self.disconnect()
        print("[DONE] 下载完成")

    @staticmethod
    def _brief(text):
        """截取响应中的有效行用于报错。"""
        for line in text.splitlines():
            line = line.strip()
            if line and 'ota' not in line.lower():
                return line[:100]
        return text[:100]


def main():
    ap = argparse.ArgumentParser(description='OTA firmware downloader (CLI protocol)')
    ap.add_argument('-p', '--port', required=True, help='串口号, 如 COM3')
    ap.add_argument('-b', '--baud', type=int, default=115200, help='波特率 (默认115200)')
    ap.add_argument('-f', '--file', required=True, help='固件文件: .bin/.hex/.elf')
    ap.add_argument('-v', '--version', type=int, default=1, help='固件版本号 (默认1)')
    ap.add_argument('-a', '--activate', action='store_true',
                    help='校验通过后自动执行 ota activate')
    args = ap.parse_args()

    try:
        data, base = resolve_fw(args.file)
        if base != 0:
            slot = 'A' if base == OTA_APP_A_BASE else 'B'
            print(f"[INFO] HEX 起始地址 0x{base:08X} -> 为 {slot} 区链接的固件")
        dl = OtaDownloader(args.port, args.baud)
        dl.run(data, args.version, args.activate)
    except Exception as e:
        print(f"\n[ERROR] {e}")
        sys.exit(1)


if __name__ == '__main__':
    main()
