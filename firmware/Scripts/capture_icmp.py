#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
=============================================================================
File Name : capture_icmp.py
Function  : PC 端 ICMP/ARP 抓包分析脚本（原始套接字方式，无需安装 Wireshark）
用途      : 分析设备(192.168.10.101) 与 PC(192.168.10.100) 之间的
            ICMP 请求/应答是否真实到达 PC 网卡
用法      : 必须以【管理员身份】运行（Windows 原始套接字需要提权）：
              python Scripts\\capture_icmp.py [秒数]
            运行后请在设备 CLI 执行:  ping 192.168.10.100
作者      : lwip_test_author
日期      : 2026-08-12
=============================================================================
"""
import socket
import struct
import sys
import time

DEV_IP   = "192.168.10.101"
PC_IP    = "192.168.10.100"
DURATION = int(sys.argv[1]) if len(sys.argv) > 1 else 20

ICMP_TYPE_NAMES = {
    0: "Echo Reply",
    8: "Echo Request",
    3: "Dest Unreachable",
    5: "Redirect",
    11: "Time Exceeded",
}


def parse_ip_header(buf):
    """解析 IPv4 头（固定 20 字节，不带选项），返回 (ihl, proto, src, dst)。"""
    if len(buf) < 20:
        return None
    ihl = (buf[0] & 0x0F) * 4            # 首部长度（字节）
    proto = buf[9]                       # 协议号：1=ICMP
    src = socket.inet_ntoa(buf[12:16])
    dst = socket.inet_ntoa(buf[16:20])
    return (ihl, proto, src, dst)


def parse_icmp_header(buf, ihl):
    """解析 ICMP 头（在 IP 头之后），返回 (type, code, id, seq)。"""
    if len(buf) < ihl + 8:
        return None
    off = ihl
    icmp_type = buf[off]
    icmp_code = buf[off + 1]
    icmp_id, icmp_seq = struct.unpack("!HH", buf[off + 4:off + 8])
    return (icmp_type, icmp_code, icmp_id, icmp_seq)


def main():
    # Windows 上创建原始套接字必须管理员权限，失败时给出明确提示
    try:
        sock = socket.socket(socket.AF_INET, socket.SOCK_RAW, socket.IPPROTO_ICMP)
    except PermissionError:
        print("[ERROR] 创建原始套接字被拒绝：请以【管理员身份】运行本脚本！")
        sys.exit(1)
    except OSError as e:
        print("[ERROR] 创建原始套接字失败: %s" % e)
        sys.exit(1)

    sock.settimeout(1.0)

    print("=" * 60)
    print("开始抓取 ICMP 流量 %d 秒（设备端请执行: ping %s）" % (DURATION, PC_IP))
    print("=" * 60)

    deadline = time.time() + DURATION
    count_req = 0
    count_rsp = 0
    frames = []          # 保存与本测试相关的帧，便于最后汇总
    other_seen = set()   # 防刷屏：只显示一次无关流量提示

    while time.time() < deadline:
        try:
            data, _addr = sock.recvfrom(65535)
        except socket.timeout:
            continue
        except OSError:
            break

        hdr = parse_ip_header(data)
        if hdr is None:
            continue
        ihl, proto, src, dst = hdr
        if proto != 1:          # 只关心 ICMP
            continue

        icmp = parse_icmp_header(data, ihl)
        if icmp is None:
            continue
        t, code, icmp_id, icmp_seq = icmp

        # 只记录与设备相关的 ICMP（源或目的为设备/PC 有线 IP）
        if (src not in (DEV_IP, PC_IP)) and (dst not in (DEV_IP, PC_IP)):
            key = (src, dst, t)
            if key not in other_seen:
                print("[SKIP] 无关 ICMP: %s -> %s %s" % (src, dst, ICMP_TYPE_NAMES.get(t, "Type%d" % t)))
                other_seen.add(key)
            continue

        name = ICMP_TYPE_NAMES.get(t, "Type%d" % t)
        ts = "%.3f" % (time.time())
        line = "[%s] %s -> %s  %s  id=%d seq=%d" % (ts, src, dst, name, icmp_id, icmp_seq)
        print(line)
        frames.append(line)
        if t == 8:
            count_req += 1
        elif t == 0:
            count_rsp += 1

    sock.close()

    print("\n" + "=" * 60)
    print("抓包结束统计：")
    print("  Echo Request (设备->PC, 类型8): %d 条" % count_req)
    print("  Echo Reply   (PC->设备, 类型0): %d 条" % count_rsp)
    print("=" * 60)
    print("\n结果判读：")
    if count_req == 0 and count_rsp == 0:
        print("  ① 未见任何设备相关 ICMP 流量 -> 设备的帧没到 PC 网卡，问题在设备发送侧")
    elif count_req > 0 and count_rsp == 0:
        print("  ② 收到设备的 Echo Request，但 PC 未回复 -> 问题在 PC 侧（回包被丢弃/未生成）")
    elif count_req > 0 and count_rsp > 0:
        print("  ③ PC 已回复 Echo Reply -> 问题在设备接收/解析侧（RAW netconn 未收到回复）")
    else:
        print("  ④ 只收到 PC 回复、未见设备请求 -> 时序交错，建议加大抓包窗口重试")


if __name__ == "__main__":
    main()
