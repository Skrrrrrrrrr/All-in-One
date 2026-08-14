#!/usr/bin/env python3
"""
LwIP Network Test Suite
=======================

功能：通过串口 CLI 对 STM32 上的 LwIP 协议栈进行自动化网络测试。

测试内容（固件端命令见 Drivers/BSP/Src/lwip_test_cmds.c）：
  1. ifconfig                    - 网络接口状态（IP/掩码/网关/MAC/链路）
  2. arp                        - ARP 表（稳定条目）
  3. route                      - 路由信息（默认出口 + 网卡列表）
  4. ping <pc_ip> [count]       - ICMP 回显测试（依赖 LWIP_RAW）
  5. tcp_test <pc_ip> <port> <len> - TCP 回环测试（本脚本运行 TCP echo 服务）
  6. udp_test <pc_ip> <port> <len> - UDP 回环测试（本脚本运行 UDP echo 服务）
  7. lwip_test <pc_ip> [...]    - 上述测试的集成执行

使用方法：
  1. 确认 PC 与开发板处于同一网段（开发板默认静态 IP：192.168.11.101）
  2. 编译烧录固件，打开串口终端确认出现 "CLI task started" 与 "> " 提示符
  3. 运行本脚本：
     python scripts/lwip_test.py -p COM3 -b 115200
     python scripts/lwip_test.py -p COM3 --device-ip 192.168.11.101 --pc-ip 192.168.11.100
     python scripts/lwip_test.py -p COM3 -m tcp --len 512 -o report.md

参数说明：
  -p, --port        串口端口（默认 COM3）
  -b, --baud        波特率（默认 115200）
  --device-ip       开发板静态 IP（默认 192.168.11.101，用于自动探测 PC 网卡 IP）
  --pc-ip           PC 端 IP（不指定则自动探测与开发板同网段的本机地址）
  --tcp-port        TCP echo 服务端口（默认 8000）
  --udp-port        UDP echo 服务端口（默认 8001）
  --len             收发负载长度（默认 256，上限 1024）
  --ping-count      ping 次数（默认 4）
  -m, --mode        测试模式：suite（集成，默认）、single（逐条）、
                    ifconfig/arp/route/ping/tcp/udp
  -o, --output      测试报告输出文件（默认 Scripts/lwip_test_report.md）

说明：
  - 脚本会在 PC 端启动 TCP/UDP echo 服务器，用于验证固件 TCP/UDP 收发链路。
  - 若测试失败，请检查 Windows 防火墙是否放行入站 TCP/UDP（可临时关闭或添加规则）。
  - 固件输出关键字：NETIF: / ARP: / ROUTE / PING: / TCP_TEST: / UDP_TEST: / LWIP_TEST_SUITE: / RESULT: PASS
"""

import argparse
import os
import re
import socket
import sys
import threading
import time

try:
    import serial
except ImportError:
    print("[ERROR] pyserial not installed. Run: pip install pyserial")
    sys.exit(1)

# 与固件 lwip_test_cmds.c 中保持一致的默认值
DEFAULT_TCP_PORT = 8000
DEFAULT_UDP_PORT = 8001
DEFAULT_DEVICE_IP = "192.168.11.101"
MAX_PAYLOAD = 1024
ECHO_END_MARKER = "[Press ENTER to execute the previous command again]"


def auto_detect_pc_ip(device_ip):
    """自动探测能到达开发板的本机 IP 地址。

    原理：创建 UDP socket 并 connect 到目标 IP，系统会为路由选择本机源地址，
    再通过 getsockname 读取该地址。UDP 的 connect 不发送数据包，目标不可达也成功。
    """
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect((device_ip, 1))
        return s.getsockname()[0]
    except OSError as e:
        print(f"[WARN] 自动探测 PC IP 失败: {e}")
        return None
    finally:
        s.close()


class TcpEchoServer:
    """TCP echo 服务器：接收任意连接并原样回显所有数据。"""

    def __init__(self, port):
        self.port = port
        self.sock = None
        self.running = False
        self.conn_count = 0
        self.error = None

    def _handle(self, conn):
        with conn:
            self.conn_count += 1
            while self.running:
                try:
                    data = conn.recv(4096)
                    if not data:
                        break
                    conn.sendall(data)
                except OSError:
                    break

    def start(self):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.sock.bind(("0.0.0.0", self.port))
        self.sock.listen(4)
        self.sock.settimeout(0.5)
        self.running = True
        self.thread = threading.Thread(target=self._accept_loop, daemon=True)
        self.thread.start()
        print(f"[OK] TCP echo server listening on 0.0.0.0:{self.port}")

    def _accept_loop(self):
        while self.running:
            try:
                conn, _ = self.sock.accept()
            except socket.timeout:
                continue
            except OSError:
                break
            t = threading.Thread(target=self._handle, args=(conn,), daemon=True)
            t.start()

    def stop(self):
        self.running = False
        if self.sock:
            try:
                self.sock.close()
            except OSError:
                pass


class UdpEchoServer:
    """UDP echo 服务器：收到数据报后原样回显给发送方。"""

    def __init__(self, port):
        self.port = port
        self.sock = None
        self.running = False
        self.count = 0
        self.error = None

    def start(self):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.sock.bind(("0.0.0.0", self.port))
        self.sock.settimeout(0.5)
        self.running = True
        self.thread = threading.Thread(target=self._recv_loop, daemon=True)
        self.thread.start()
        print(f"[OK] UDP echo server listening on 0.0.0.0:{self.port}")

    def _recv_loop(self):
        while self.running:
            try:
                data, addr = self.sock.recvfrom(4096)
            except socket.timeout:
                continue
            except OSError:
                break
            self.count += 1
            try:
                self.sock.sendto(data, addr)
            except OSError:
                pass

    def stop(self):
        self.running = False
        if self.sock:
            try:
                self.sock.close()
            except OSError:
                pass


class LwipTestRunner:
    """串口 CLI 测试执行器：发送命令、捕获输出、校验关键字。"""

    def __init__(self, port, baud, device_ip, pc_ip, tcp_port, udp_port, payload_len, ping_count):
        self.port = port
        self.baud = baud
        self.device_ip = device_ip
        self.pc_ip = pc_ip
        self.tcp_port = tcp_port
        self.udp_port = udp_port
        self.payload_len = min(max(payload_len, 1), MAX_PAYLOAD)
        self.ping_count = min(max(ping_count, 1), 100)
        self.ser = None
        self.buf = ""                 # 累积的原始串口文本
        self.buf_lock = threading.Lock()
        self.results = {}

    # ---------------- 串口连接 ----------------
    def connect(self, max_retries=5, retry_delay=2):
        for attempt in range(1, max_retries + 1):
            try:
                if self.ser and self.ser.is_open:
                    self.ser.close()
                self.ser = serial.Serial(port=self.port, baudrate=self.baud,
                                         parity='N', stopbits=1, bytesize=8, timeout=0.1)
                print(f"[OK] 已连接 {self.port} @ {self.baud} bps (attempt {attempt})")
                return True
            except serial.SerialException as e:
                print(f"[WARN] 连接失败 (attempt {attempt}/{max_retries}): {e}")
                if attempt < max_retries:
                    time.sleep(retry_delay)
        print("[FAIL] 无法打开串口，请确认端口号及占用情况")
        return False

    def disconnect(self):
        if self.ser and self.ser.is_open:
            self.ser.close()
        print("[INFO] 已断开串口")

    # ---------------- 输出捕获 ----------------
    def _pump(self, duration=0.2):
        """读取一段时间内的串口数据并追加到缓冲。"""
        end = time.time() + duration
        while time.time() < end:
            try:
                n = self.ser.in_waiting
                if n > 0:
                    data = self.ser.read(n).decode('utf-8', errors='replace')
                    with self.buf_lock:
                        self.buf += data
            except serial.SerialException:
                break
            time.sleep(0.005)

    def _wait_marker(self, marker, timeout):
        """阻塞等待缓冲中出现指定标记，返回是否成功。"""
        end = time.time() + timeout
        while time.time() < end:
            with self.buf_lock:
                if marker in self.buf:
                    return True
            self._pump(0.05)
        return False

    def _strip_ansi(self, text):
        """去除 ANSI 转义序列（CLI 红字/光标控制）。"""
        return re.sub(r'\x1B\[[0-9;]*[A-Za-z]', '', text)

    def send_cmd(self, cmd, timeout=15):
        """发送命令并返回该命令的输出文本。

        以 CLI 固定输出的 "[Press ENTER..." 作为命令执行结束标记，
        该标记位于命令输出之后、下一个提示符之前。
        注意：缓冲中可能残留上次命令的标记，因此必须等待"标记计数增加"
        而非"标记出现"，否则第二条命令会立即误判为已完成。
        """
        with self.buf_lock:
            baseline_len = len(self.buf)
            base_markers = self.buf.count(ECHO_END_MARKER)

        self.ser.write((cmd + '\r\n').encode('utf-8'))
        ok = self._wait_marker_count(base_markers, timeout)
        self._pump(0.3)  # 冲刷剩余输出

        with self.buf_lock:
            out = self.buf[baseline_len:]
        return self._strip_ansi(out), ok

    def _wait_marker_count(self, base_markers, timeout):
        """阻塞等待缓冲中结束标记出现次数超过 base_markers，返回是否成功。"""
        end = time.time() + timeout
        while time.time() < end:
            with self.buf_lock:
                if self.buf.count(ECHO_END_MARKER) > base_markers:
                    return True
            self._pump(0.05)
        return False

    def wait_prompt(self, timeout=10):
        """等待设备打印出 CLI 提示符。"""
        self.ser.write(b'\r\n')
        ok = self._wait_marker(ECHO_END_MARKER, timeout) or \
             self._wait_marker('> ', timeout)
        self._pump(0.3)
        with self.buf_lock:
            text = self.buf
        print(f"[INFO] 启动输出（前 300 字符）:\n{self._strip_ansi(text)[:300]}")
        return ok

    # ---------------- 单项测试 ----------------
    def test_ifconfig(self):
        print("\n" + "=" * 60)
        print("TEST: ifconfig")
        print("=" * 60)
        out, ok = self.send_cmd('ifconfig')
        if not ok:
            print("  [FAIL] ifconfig 命令无响应")
            return False
        print("  " + '\n  '.join(l for l in out.splitlines() if l.strip()))

        net_ok = ('NETIF:' in out) and ('LINK=UP' in out or 'link=UP' in out)
        phy_ok = 'PHY: link=UP' in out
        passed = net_ok and phy_ok
        print(f"  [{'OK' if net_ok else 'FAIL'}] 接口状态 UP/LINK=UP")
        print(f"  [{'OK' if phy_ok else 'FAIL'}] PHY 链路 link=UP")
        return passed

    def test_arp(self):
        print("\n" + "=" * 60)
        print("TEST: arp")
        print("=" * 60)
        out, ok = self.send_cmd('arp')
        if not ok:
            print("  [FAIL] arp 命令无响应")
            return False
        print("  " + '\n  '.join(l for l in out.splitlines() if l.strip()))

        table_ok = ('ARP Table' in out) and ('stable entries' in out)
        netif_ok = 'NETIF:' in out
        passed = table_ok and netif_ok
        print(f"  [{'OK' if table_ok else 'FAIL'}] ARP 表输出（stable entries）")
        print(f"  [{'OK' if netif_ok else 'FAIL'}] NETIF 信息")
        return passed

    def test_route(self):
        print("\n" + "=" * 60)
        print("TEST: route")
        print("=" * 60)
        out, ok = self.send_cmd('route')
        if not ok:
            print("  [FAIL] route 命令无响应")
            return False
        print("  " + '\n  '.join(l for l in out.splitlines() if l.strip()))

        default_ok = ('Routing Table' in out) and ('DEFAULT' in out)
        iface_ok = 'interface(s)' in out
        passed = default_ok and iface_ok
        print(f"  [{'OK' if default_ok else 'FAIL'}] 默认出口 DEFAULT 输出")
        print(f"  [{'OK' if iface_ok else 'FAIL'}] 网卡列表输出")
        return passed

    def test_ping(self):
        print("\n" + "=" * 60)
        print(f"TEST: ping {self.pc_ip} x{self.ping_count}")
        print("=" * 60)
        out, ok = self.send_cmd(f'ping {self.pc_ip} {self.ping_count}',
                                timeout=20 + self.ping_count * 2)
        if not ok:
            print("  [FAIL] ping 命令无响应")
            return False
        print("  " + '\n  '.join(l for l in out.splitlines() if 'PING' in l or 'ping' in l))

        m = re.search(r'PING: sent=(\d+) recv=(\d+) lost=(\d+)', out)
        if not m:
            print("  [FAIL] 未解析到 PING 结果")
            return False
        sent, recv, lost = int(m.group(1)), int(m.group(2)), int(m.group(3))
        passed = (recv > 0) and (lost == 0)
        print(f"  [INFO] sent={sent} recv={recv} lost={lost}")
        print(f"  [{'OK' if passed else 'FAIL'}] 全部回显成功")
        return passed

    def test_tcp(self):
        print("\n" + "=" * 60)
        print(f"TEST: tcp_test {self.pc_ip}:{self.tcp_port} len={self.payload_len}")
        print("=" * 60)
        out, ok = self.send_cmd(f'tcp_test {self.pc_ip} {self.tcp_port} {self.payload_len}', timeout=20)
        if not ok:
            print("  [FAIL] tcp_test 命令无响应")
            return False
        print("  " + '\n  '.join(l for l in out.splitlines() if 'TCP_TEST' in l))

        m = re.search(r'TCP_TEST: connect=(\w+) rtt=(\d+) ms sent=(\d+) recv=(\d+) verify=(\w+)', out)
        if not m:
            print("  [FAIL] 未解析到 TCP_TEST 结果")
            return False
        conn, rtt, sent, recv, verify = m.group(1), int(m.group(2)), int(m.group(3)), int(m.group(4)), m.group(5)
        passed = (conn == 'OK') and (recv >= sent) and (verify == 'OK')
        print(f"  [INFO] connect={conn} rtt={rtt}ms sent={sent} recv={recv} verify={verify}")
        print(f"  [{'OK' if passed else 'FAIL'}] TCP 回环校验通过")
        return passed

    def test_udp(self):
        print("\n" + "=" * 60)
        print(f"TEST: udp_test {self.pc_ip}:{self.udp_port} len={self.payload_len}")
        print("=" * 60)
        out, ok = self.send_cmd(f'udp_test {self.pc_ip} {self.udp_port} {self.payload_len}', timeout=15)
        if not ok:
            print("  [FAIL] udp_test 命令无响应")
            return False
        print("  " + '\n  '.join(l for l in out.splitlines() if 'UDP_TEST' in l))

        m = re.search(r'UDP_TEST: rtt=(\d+) ms sent=(\d+) recv=(\d+) verify=(\w+)', out)
        if not m:
            print("  [FAIL] 未解析到 UDP_TEST 结果")
            return False
        rtt, sent, recv, verify = int(m.group(1)), int(m.group(2)), int(m.group(3)), m.group(4)
        passed = (recv >= sent) and (verify == 'OK')
        print(f"  [INFO] rtt={rtt}ms sent={sent} recv={recv} verify={verify}")
        print(f"  [{'OK' if passed else 'FAIL'}] UDP 回环校验通过")
        return passed

    def test_suite(self):
        print("\n" + "=" * 60)
        print(f"TEST: lwip_test 集成套件（ifconfig+ping+tcp+udp）")
        print("=" * 60)
        out, ok = self.send_cmd(f'lwip_test {self.pc_ip} {self.tcp_port} {self.udp_port} {self.payload_len}', timeout=60)
        if not ok:
            print("  [FAIL] lwip_test 命令无响应")
            return False
        for line in out.splitlines():
            if any(k in line for k in ('NETIF:', 'PHY:', 'PING:', 'TCP_TEST:', 'UDP_TEST:', 'RESULT:')):
                print("  " + line)

        suite_ok = 'LWIP_TEST_SUITE: netif=PASS ping=PASS tcp=PASS udp=PASS' in out
        result_ok = 'RESULT: PASS' in out
        passed = suite_ok and result_ok
        print(f"  [{'OK' if suite_ok else 'FAIL'}] LWIP_TEST_SUITE 四项全 PASS")
        return passed

    # ---------------- 报告与主流程 ----------------
    def generate_report(self, output_file):
        report = []
        report.append("# LwIP Network Test Report")
        report.append("")
        report.append("## Test Configuration")
        report.append(f"- **Test Date**: {time.strftime('%Y-%m-%d %H:%M:%S')}")
        report.append(f"- **Serial Port**: {self.port} @ {self.baud}")
        report.append(f"- **Device IP**: {self.device_ip}")
        report.append(f"- **PC IP**: {self.pc_ip}")
        report.append(f"- **TCP Port / UDP Port**: {self.tcp_port} / {self.udp_port}")
        report.append(f"- **Payload Length**: {self.payload_len}")
        report.append(f"- **Ping Count**: {self.ping_count}")
        report.append("")
        report.append("## Test Results")
        report.append("")
        report.append("| Check | Result |")
        report.append("|-------|--------|")
        for name, ok in self.results.items():
            report.append(f"| {name} | {'✅' if ok else '❌'} |")
        report.append("")
        all_ok = all(self.results.values())
        report.append(f"**RESULT: {'PASS' if all_ok else 'FAIL'}** ({sum(self.results.values())}/{len(self.results)})")
        report.append("")
        report.append("## Notes")
        report.append("")
        report.append("- ifconfig: 依赖 netif_default 与 DP83848 PHY 驱动")
        report.append("- arp: 依赖 LWIP_ARP；etharp_get_entry 仅返回 STABLE 及以上条目")
        report.append("- route: 遍历 netif_list，依赖 netif_default")
        report.append("- ping: 依赖 LWIP_RAW=1（lwipopts.h USER CODE 中使能）")
        report.append("- tcp/udp: PC 端由本脚本提供 echo 服务，失败时检查 Windows 防火墙")
        with open(output_file, 'w', encoding='utf-8') as f:
            f.write('\n'.join(report))
        print(f"[INFO] 报告已保存: {output_file}")

    def run(self, mode, output_file):
        if not self.connect():
            return False

        tcp_srv = TcpEchoServer(self.tcp_port)
        udp_srv = UdpEchoServer(self.udp_port)
        tcp_srv.start()
        udp_srv.start()

        print("\n--- 等待设备 CLI 提示符 ---")
        if not self.wait_prompt(timeout=10):
            print("[FAIL] 未检测到 CLI 提示符，请确认固件已烧录且串口正确")
            tcp_srv.stop(); udp_srv.stop(); self.disconnect()
            return False
        print("[OK] CLI 就绪")

        self.results = {}
        if mode == 'ifconfig':
            self.results['ifconfig'] = self.test_ifconfig()
        elif mode == 'arp':
            self.results['arp'] = self.test_arp()
        elif mode == 'route':
            self.results['route'] = self.test_route()
        elif mode == 'ping':
            self.results['ping'] = self.test_ping()
        elif mode == 'tcp':
            self.results['tcp'] = self.test_tcp()
        elif mode == 'udp':
            self.results['udp'] = self.test_udp()
        elif mode in ('single', 'all'):
            self.results['ifconfig'] = self.test_ifconfig()
            self.results['arp'] = self.test_arp()
            self.results['route'] = self.test_route()
            self.results['ping'] = self.test_ping()
            self.results['tcp'] = self.test_tcp()
            self.results['udp'] = self.test_udp()
        else:  # suite（默认）
            self.results['ifconfig'] = self.test_ifconfig()
            self.results['arp'] = self.test_arp()
            self.results['route'] = self.test_route()
            self.results['suite'] = self.test_suite()

        tcp_srv.stop()
        udp_srv.stop()
        self.disconnect()

        print("\n" + "=" * 60)
        print("SUMMARY")
        print("=" * 60)
        passed = 0
        for name, ok in self.results.items():
            print(f"  [{'OK' if ok else 'FAIL'}] {name}")
            if ok:
                passed += 1
        all_ok = passed == len(self.results)
        print(f"\nPassed: {passed}/{len(self.results)}")
        print(f"RESULT: {'PASS' if all_ok else 'FAIL'}")

        self.generate_report(output_file)
        return all_ok


def main():
    script_dir = os.path.dirname(os.path.abspath(__file__))
    parser = argparse.ArgumentParser(description='LwIP Network Test Suite')
    parser.add_argument('-p', '--port', default='COM3', help='Serial port (default: COM3)')
    parser.add_argument('-b', '--baud', type=int, default=115200, help='Baud rate (default: 115200)')
    parser.add_argument('--device-ip', default=DEFAULT_DEVICE_IP, help='Device static IP')
    parser.add_argument('--pc-ip', default=None, help='PC IP (auto-detect if omitted)')
    parser.add_argument('--tcp-port', type=int, default=DEFAULT_TCP_PORT, help='TCP echo port')
    parser.add_argument('--udp-port', type=int, default=DEFAULT_UDP_PORT, help='UDP echo port')
    parser.add_argument('--len', type=int, default=256, help='Payload length (1-1024)')
    parser.add_argument('--ping-count', type=int, default=4, help='Ping count (1-100)')
    parser.add_argument('-m', '--mode', default='suite',
                        choices=['suite', 'single', 'all', 'ifconfig',
                                 'arp', 'route', 'ping', 'tcp', 'udp'],
                        help='Test mode')
    parser.add_argument('-o', '--output', default=os.path.join(script_dir, 'lwip_test_report.md'),
                        help='Report output file')
    args = parser.parse_args()

    pc_ip = args.pc_ip or auto_detect_pc_ip(args.device_ip)
    if not pc_ip:
        print("[FAIL] 无法自动探测 PC IP，请通过 --pc-ip 显式指定")
        sys.exit(1)
    print(f"[INFO] PC IP: {pc_ip}  ->  设备 IP: {args.device_ip}")

    runner = LwipTestRunner(args.port, args.baud, args.device_ip, pc_ip,
                            args.tcp_port, args.udp_port, args.len, args.ping_count)
    success = runner.run(args.mode, args.output)
    sys.exit(0 if success else 1)


if __name__ == "__main__":
    main()
