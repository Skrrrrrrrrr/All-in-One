#!/usr/bin/env python3
"""
STM32 Comprehensive Test Suite
==============================

功能：对STM32系统进行全面的功能测试、压力测试和可靠性测试

测试类型：
  1. 功能测试 - 验证所有CLI命令正常工作
  2. 命令压力测试 - 高频率连续发送命令
  3. 并发测试 - 多线程发送命令（使用命令队列确保串行化）
  4. 缓冲区压力测试 - 大量数据传输测试
  5. 可靠性测试 - 持续运行测试
  6. PVD测试 - 电源电压检测模拟测试

使用方法：
  python scripts/test_stm32.py -p COM3 -b 115200 -m all
  python scripts/test_stm32.py -p COM3 -b 115200 -m stress -c 500
  python scripts/test_stm32.py -p COM3 -b 115200 -m pvd
  python scripts/test_stm32.py -p COM3 -b 115200 -m reliability -d 300

参数说明：
  -p, --port     串口端口（默认 COM3）
  -b, --baud     波特率（默认 115200）
  -m, --mode     测试模式：all（全部）、functional（功能）、stress（压力）、
                 concurrent（并发）、buffer（缓冲区）、reliability（可靠性）、pvd
  -d, --duration 测试时长（默认 300 秒）
  -t, --threads  并发线程数（默认 4）
  -c, --count    每个线程发送命令次数（默认 500）
  -o, --output   测试报告输出文件（默认 scripts/test_report.md）

测试指标：
  - 命令响应时间（RTT）
  - 命令成功率
  - 缓冲区溢出情况
  - 系统稳定性（HardFault检测）
  - PVD ISR执行流程验证
"""

import serial
import time
import threading
import queue
import random
import argparse
import sys
import os
import re

try:
    import win32com.client
    HAS_WIN32 = True
except ImportError:
    HAS_WIN32 = False

class CLITestSuite:
    def __init__(self, port='COM3', baud=115200):
        self.port = port
        self.baud = baud
        self.ser = None
        self.running = False
        self.received_lines = []
        self.prompt_count = 0
        self.hardfault_count = 0
        self.buffer_full_count = 0
        self.lock = threading.Lock()
        self.command_queue = queue.Queue()
        
        self.commands = [
            'help',
            'tasks',
            'stats',
            'heap',
            'pvd status',
            'log tail',
            'log tail 10',
            'log tail 50',
        ]
        
        self.results = {
            'total_commands': 0,
            'successful_commands': 0,
            'failed_commands': 0,
            'rtt_values': [],
            'hardfault_count': 0,
            'buffer_full_count': 0,
            'errors': [],
        }
    
    def connect(self, max_retries=5, retry_delay=3):
        for attempt in range(1, max_retries + 1):
            try:
                if self.ser and self.ser.is_open:
                    self.ser.close()
                
                self.ser = serial.Serial(
                    port=self.port,
                    baudrate=self.baud,
                    parity='N',
                    stopbits=1,
                    bytesize=8,
                    timeout=0.1
                )
                print(f"[OK] Connected to {self.port} at {self.baud} bps (attempt {attempt}/{max_retries})")
                return True
            except PermissionError as e:
                print(f"[WARN] Attempt {attempt}/{max_retries} - Permission error: {e}")
                print(f"[INFO] Port {self.port} may be in use by another program")
                self.log_error(f"Connection attempt {attempt}: PermissionError - {e}")
                if attempt < max_retries:
                    print(f"[INFO] Retrying in {retry_delay} seconds...")
                    time.sleep(retry_delay)
            except serial.SerialException as e:
                print(f"[WARN] Attempt {attempt}/{max_retries} - Serial error: {e}")
                self.log_error(f"Connection attempt {attempt}: SerialException - {e}")
                if attempt < max_retries:
                    print(f"[INFO] Retrying in {retry_delay} seconds...")
                    time.sleep(retry_delay)
            except Exception as e:
                print(f"[ERROR] Attempt {attempt}/{max_retries} - Unexpected error: {e}")
                self.log_error(f"Connection attempt {attempt}: Unexpected error - {e}")
                break
        
        print(f"[FAIL] Failed to connect after {max_retries} attempts")
        self.log_error(f"Failed to connect to {self.port} after {max_retries} attempts")
        return False
    
    def log_error(self, message):
        timestamp = time.strftime('%Y-%m-%d %H:%M:%S')
        error_entry = f"[{timestamp}] {message}"
        self.results['errors'].append(error_entry)
        print(f"[ERROR LOG] {error_entry}")
    
    def disconnect(self):
        if self.ser:
            self.ser.close()
            print("[INFO] Disconnected")
    
    def check_connectivity(self):
        print("[INFO] Checking connectivity...")
        
        start_time = time.time()
        while time.time() - start_time < 10:
            if self.ser.in_waiting > 0:
                data = self.ser.read(self.ser.in_waiting).decode('utf-8', errors='ignore')
                if '>' in data:
                    print("[OK] Prompt detected")
                    return True
                print(f"[INFO] Received: {repr(data[:100])}")
            
            self.ser.write(b'\r\n')
            time.sleep(0.5)
        
        print("[FAIL] No prompt received within 10 seconds")
        return False
    
    def read_thread(self):
        buffer = ""
        while self.running:
            try:
                if self.ser and self.ser.in_waiting > 0:
                    data = self.ser.read(self.ser.in_waiting).decode('utf-8', errors='ignore')
                    buffer += data
                    while '\n' in buffer:
                        idx = buffer.find('\n')
                        line = buffer[:idx]
                        buffer = buffer[idx+1:]
                        sublines = line.split('\r')
                        for sub in sublines:
                            sub = sub.strip()
                            if sub:
                                self.process_line(sub)
                time.sleep(0.005)
            except Exception as e:
                print(f"[ERROR] Read error: {e}")
                break
    
    def process_line(self, line):
        with self.lock:
            self.received_lines.append(line)
            
            if 'HardFault' in line:
                self.hardfault_count += 1
                print(f"[{time.time():.3f}] [ERROR] HardFault detected!")
            elif line.strip() == '>' or line.strip() == '> ' or '> ' in line or line.endswith('>'):
                self.prompt_count += 1
                print(f"[{time.time():.3f}] [DEBUG] Prompt detected: {repr(line)}")
            elif 'Buffer full count:' in line:
                try:
                    count = int(line.split(':')[1].strip())
                    if count > 0:
                        self.buffer_full_count += 1
                        print(f"[{time.time():.3f}] [WARN] Buffer overflow detected: {line}")
                except:
                    pass
    
    def send_command_blocking(self, cmd, timeout=5, max_retries=2):
        if not self.ser or not self.running:
            print(f"[{time.time():.3f}] [DEBUG] send_command_blocking: ser={self.ser}, running={self.running}")
            return False, 0
        
        is_log_tail = 'log tail' in cmd
        
        for attempt in range(1, max_retries + 1):
            send_time = time.time()
            
            try:
                with self.lock:
                    target_prompt = self.prompt_count + 1
                    if is_log_tail:
                        print(f"[{time.time():.3f}] [DEBUG] >>> PREPARING TO SEND 'log tail' (attempt {attempt}) <<<")
                        print(f"[{time.time():.3f}] [DEBUG] Current prompt count: {self.prompt_count}, Target: {target_prompt}")
                        print(f"[{time.time():.3f}] [DEBUG] Last 5 received lines: {self.received_lines[-5:] if len(self.received_lines) >= 5 else self.received_lines}")
                    
                    print(f"[{time.time():.3f}] [DEBUG] Sending command: '{cmd[:30]}...', target_prompt={target_prompt}, current_prompt={self.prompt_count}")
                    self.ser.write((cmd + '\r\n').encode('utf-8'))
                
                start_time = time.time()
                last_prompt_count = -1
                while time.time() - start_time < timeout:
                    with self.lock:
                        current_prompt = self.prompt_count
                        current_lines = len(self.received_lines)
                    
                    if current_prompt > last_prompt_count:
                        last_prompt_count = current_prompt
                        if is_log_tail:
                            print(f"[{time.time():.3f}] [DEBUG] log tail - prompt count changed: {current_prompt}, total lines: {current_lines}")
                    
                    if is_log_tail and current_lines > 0:
                        last_lines = self.received_lines[-3:] if len(self.received_lines) >= 3 else self.received_lines
                        if any('HardFault' in line for line in last_lines):
                            print(f"[{time.time():.3f}] [DEBUG] log tail - HardFault detected in recent lines!")
                    
                    if current_prompt >= target_prompt:
                        rtt = (time.time() - send_time) * 1000
                        if is_log_tail:
                            print(f"[{time.time():.3f}] [DEBUG] >>> 'log tail' COMPLETED (attempt {attempt}) <<<")
                            print(f"[{time.time():.3f}] [DEBUG] RTT={rtt:.1f}ms, final prompt count: {current_prompt}")
                            print(f"[{time.time():.3f}] [DEBUG] Total received lines during command: {current_lines}")
                        
                        print(f"[{time.time():.3f}] [DEBUG] Command '{cmd[:20]}...' succeeded, RTT={rtt:.1f}ms")
                        return True, rtt
                    
                    time.sleep(0.01)
                
                if is_log_tail:
                    print(f"[{time.time():.3f}] [DEBUG] >>> 'log tail' TIMED OUT (attempt {attempt}) <<<")
                    print(f"[{time.time():.3f}] [DEBUG] Timeout: {timeout}s, last prompt count: {last_prompt_count}")
                    print(f"[{time.time():.3f}] [DEBUG] Last 10 received lines: {self.received_lines[-10:] if len(self.received_lines) >= 10 else self.received_lines}")
                    print(f"[{time.time():.3f}] [DEBUG] HardFault count: {self.hardfault_count}")
                
                print(f"[{time.time():.3f}] [DEBUG] Command '{cmd[:20]}...' timed out after {timeout}s (attempt {attempt}/{max_retries})")
                
                if attempt < max_retries:
                    print(f"[{time.time():.3f}] [INFO] Retrying command '{cmd[:20]}...' (attempt {attempt+1}/{max_retries})")
                    time.sleep(0.5)
                    continue
                
                self.log_error(f"Command timeout: '{cmd[:30]}...' after {timeout}s, {max_retries} attempts")
                return False, (time.time() - send_time) * 1000
            
            except serial.SerialException as e:
                if is_log_tail:
                    print(f"[{time.time():.3f}] [DEBUG] >>> 'log tail' SERIAL EXCEPTION (attempt {attempt}) <<<")
                    print(f"[{time.time():.3f}] [ERROR] Serial Exception: {e}")
                print(f"[{time.time():.3f}] [ERROR] Serial error sending command: {e}")
                self.log_error(f"Serial error: '{cmd[:30]}...' - {e}")
                
                if attempt < max_retries:
                    print(f"[{time.time():.3f}] [INFO] Reconnecting and retrying...")
                    self.disconnect()
                    if self.connect():
                        print(f"[{time.time():.3f}] [INFO] Reconnected successfully")
                        continue
                
                return False, 0
            except Exception as e:
                if is_log_tail:
                    print(f"[{time.time():.3f}] [DEBUG] >>> 'log tail' EXCEPTION (attempt {attempt}) <<<")
                    print(f"[{time.time():.3f}] [ERROR] Exception: {e}")
                print(f"[{time.time():.3f}] [ERROR] Send error: {e}")
                self.log_error(f"Unexpected error: '{cmd[:30]}...' - {e}")
                return False, 0
    
    def sender_thread(self):
        cmd_counter = 0
        while self.running:
            try:
                cmd, timeout = self.command_queue.get(timeout=1)
                cmd_counter += 1
                
                print(f"[{time.time():.3f}] [DEBUG] Dequeued command #{cmd_counter}: '{cmd[:30]}...', timeout={timeout}s")
                
                success, rtt = self.send_command_blocking(cmd, timeout=timeout)
                
                with self.lock:
                    self.results['total_commands'] += 1
                    if success:
                        self.results['successful_commands'] += 1
                        if rtt > 0:
                            self.results['rtt_values'].append(rtt)
                    else:
                        self.results['failed_commands'] += 1
                
                self.command_queue.task_done()
                
                with self.lock:
                    qsize = self.command_queue.qsize()
                if qsize > 0:
                    print(f"[{time.time():.3f}] [DEBUG] Queue size: {qsize}")
                    
            except queue.Empty:
                continue
            except Exception as e:
                print(f"[{time.time():.3f}] [ERROR] Sender thread error: {e}")
                break
    
    def run_functional_test(self):
        print("\n" + "="*60)
        print("FUNCTIONAL TEST")
        print("="*60)
        
        test_commands = [
            ('help', 'Help command'),
            ('tasks', 'Tasks command'),
            ('stats', 'Stats command'),
            ('heap', 'Heap command'),
            ('flash', 'Flash command'),
            ('pvd status', 'PVD status command'),
            ('top', 'Resource/CPU usage command'),
        ]
        
        for cmd, desc in test_commands:
            self.command_queue.put((cmd, 5))
            time.sleep(0.3)
        
        self.command_queue.join()
        
        print("  [INFO] Functional test commands queued")
        return True
    
    def run_top_command_test(self):
        """Verify 'top' command output contains all expected sections."""
        print("\n" + "="*60)
        print("TOP COMMAND VERIFICATION TEST")
        print("="*60)
        
        # Record current line count to isolate top command output
        with self.lock:
            baseline = len(self.received_lines)
        
        # Send top command (needs longer timeout due to 500ms sampling)
        self.command_queue.put(('top', 10))
        self.command_queue.join()
        
        # Give some time for output to be received
        time.sleep(2)
        
        # Collect lines received after baseline
        with self.lock:
            top_output_lines = self.received_lines[baseline:]
        
        # Join all output into a single string for keyword search
        top_output = '\n'.join(top_output_lines)
        
        # Expected sections in top command output
        expected_sections = [
            ('[Heap]', 'Heap usage section'),
            ('[CPU]', 'CPU usage section'),
            ('[Flash]', 'Flash usage section'),
            ('[UART]', 'UART buffer section'),
            ('Used:', 'Heap used bytes'),
            ('CPU:', 'Per-task CPU percentage'),
            ('FreeStack:', 'Task stack high water mark'),
            ('P:', 'Task priority'),
        ]
        
        all_found = True
        for keyword, desc in expected_sections:
            if keyword in top_output:
                print(f"  [OK] Found '{keyword}' - {desc}")
            else:
                print(f"  [FAIL] Missing '{keyword}' - {desc}")
                all_found = False
        
        # Print sample of top output for visual inspection
        print("\n  [INFO] Sample top command output:")
        for line in top_output_lines[:15]:
            if line.strip() and not line.strip().startswith('>'):
                print(f"    {line}")
        
        if all_found:
            print("\n  [PASS] Top command verification passed")
        else:
            print("\n  [FAIL] Top command verification failed - missing sections")
        
        return all_found
    
    def run_clr_command_test(self):
        """Verify 'clr' command returns ANSI clear-screen escape sequence."""
        print("\n" + "="*60)
        print("CLR COMMAND VERIFICATION TEST")
        print("="*60)
        
        baseline = len(self.received_lines)
        self.command_queue.put(('clr', 5))
        self.command_queue.join()
        time.sleep(1)
        
        with self.lock:
            clr_output_lines = self.received_lines[baseline:]
        
        clr_output = '\n'.join(clr_output_lines)
        
        # ANSI escape codes expected: ESC[2J (clear), ESC[H (home cursor)
        has_ansi_clear = '\033[2J' in clr_output or '\x1b[2J' in clr_output
        has_ansi_home = '\033[H' in clr_output or '\x1b[H' in clr_output
        
        # Also check that a prompt is received after clr (terminal is still responsive)
        has_prompt = '>' in clr_output
        
        results = [
            (has_ansi_clear, 'ANSI clear-screen (ESC[2J)'),
            (has_ansi_home, 'ANSI cursor-home (ESC[H)'),
            (has_prompt, 'Prompt received after clr'),
        ]
        
        all_found = True
        for passed, desc in results:
            if passed:
                print(f"  [OK] {desc}")
            else:
                print(f"  [FAIL] {desc}")
                all_found = False
        
        if all_found:
            print("\n  [PASS] clr command verification passed")
        else:
            print("\n  [FAIL] clr command verification failed")
        
        return all_found
    
    def run_log_tail_test(self):
        print("\n" + "="*60)
        print("LOG TAIL FUNCTIONAL TEST")
        print("="*60)
        
        log_tail_tests = [
            ('log tail', 'Default tail (10 lines)'),
            ('log tail 10', 'Tail 10 lines'),
            ('log tail 20', 'Tail 20 lines'),
            ('log tail 50', 'Tail 50 lines'),
            ('log tail 100', 'Tail 100 lines'),
            ('log tail all', 'Tail all lines'),
        ]
        
        print("[INFO] Testing log tail commands...")
        
        for cmd, desc in log_tail_tests:
            print(f"  [TEST] {desc}: '{cmd}'")
            self.command_queue.put((cmd, 30))
            time.sleep(1)
        
        self.command_queue.join()
        
        print("[INFO] Log tail functional test completed")
        return True
    
    def run_log_tail_performance_test(self):
        print("\n" + "="*60)
        print("LOG TAIL PERFORMANCE TEST")
        print("="*60)
        
        test_cases = [
            (10, '10 lines'),
            (50, '50 lines'),
            (100, '100 lines'),
            (200, '200 lines'),
            (500, '500 lines'),
        ]
        
        print("[INFO] Testing log tail performance with different line counts...")
        
        tail_results = []
        
        for count, desc in test_cases:
            start_time = time.time()
            cmd = f'log tail {count}'
            print(f"  [TEST] {desc}: '{cmd}'")
            
            self.command_queue.put((cmd, 60))
            self.command_queue.join()
            
            elapsed = time.time() - start_time
            tail_results.append((count, elapsed))
            print(f"  [RESULT] {desc}: {elapsed:.2f}s")
            time.sleep(0.5)
        
        print("\n[INFO] Log tail performance summary:")
        for count, elapsed in tail_results:
            print(f"  log tail {count}: {elapsed:.2f}s")
        
        return True
    
    def run_log_tail_stress_test(self):
        print("\n" + "="*60)
        print("LOG TAIL STRESS TEST")
        print("="*60)
        
        print("[INFO] Running repeated log tail commands to test stability...")
        
        test_sequence = [
            'log tail',
            'log tail 50',
            'log tail',
            'log tail 100',
            'log tail',
            'log tail 20',
        ]
        
        iterations = 20
        total_elapsed = 0
        
        for i in range(iterations):
            start_time = time.time()
            
            for cmd in test_sequence:
                self.command_queue.put((cmd, 30))
            
            self.command_queue.join()
            
            elapsed = time.time() - start_time
            total_elapsed += elapsed
            
            if (i + 1) % 5 == 0:
                avg_time = total_elapsed / (i + 1)
                print(f"  [PROGRESS] Iteration {i+1}/{iterations}, Avg time per cycle: {avg_time:.2f}s")
        
        avg_time = total_elapsed / iterations
        print(f"\n[INFO] Log tail stress test completed")
        print(f"  Total cycles: {iterations}")
        print(f"  Average time per cycle: {avg_time:.2f}s")
        print(f"  HardFault count: {self.hardfault_count}")
        
        return True
    
    def get_command_timeout(self, cmd):
        if 'flash' in cmd:
            return 15
        elif 'log tail all' in cmd:
            return 60
        elif 'log tail' in cmd:
            return 30
        elif 'tasks' in cmd:
            return 8
        elif 'stats' in cmd:
            return 8
        elif 'help' in cmd:
            return 8
        else:
            return 5
    
    def run_stress_test(self, num_commands=500, delay=0.2):
        print("\n" + "="*60)
        print("STRESS TEST")
        print("="*60)
        print(f"[INFO] Sending {num_commands} commands with {delay*1000:.1f}ms delay")
        
        start_time = time.time()
        for i in range(num_commands):
            cmd = random.choice(self.commands)
            timeout = self.get_command_timeout(cmd)
            self.command_queue.put((cmd, timeout))
            
            if (i + 1) % 100 == 0:
                elapsed = time.time() - start_time
                rate = (i + 1) / elapsed
                print(f"[INFO] Progress: {i+1}/{num_commands}, Rate: {rate:.1f} cmds/s")
            
            time.sleep(delay)
        
        self.command_queue.join()
        
        elapsed = time.time() - start_time
        print(f"[INFO] Completed {num_commands} commands in {elapsed:.1f}s")
    
    def run_concurrent_test(self, num_threads=4, num_commands=100):
        print("\n" + "="*60)
        print("CONCURRENT TEST")
        print("="*60)
        print(f"[INFO] Testing with {num_threads} concurrent threads, {num_commands} commands each")
        print(f"[INFO] log tail command monitoring enabled")
        
        log_tail_count = 0
        
        def worker(thread_id):
            nonlocal log_tail_count
            for i in range(num_commands):
                if not self.running:
                    break
                cmd = random.choice(self.commands)
                is_tail = 'log tail' in cmd
                if is_tail:
                    log_tail_count += 1
                    print(f"[{time.time():.3f}] [INFO] Worker {thread_id}: queuing log tail (total: {log_tail_count})")
                self.command_queue.put((cmd, 10))
                time.sleep(random.uniform(0.1, 0.2))
        
        threads = []
        for i in range(num_threads):
            t = threading.Thread(target=worker, args=(i,))
            t.daemon = True
            threads.append(t)
        
        start_time = time.time()
        for t in threads:
            t.start()
        
        for t in threads:
            t.join()
        
        self.command_queue.join()
        
        elapsed = time.time() - start_time
        
        print(f"[INFO] Completed {num_threads * num_commands} commands in {elapsed:.1f}s")
        print(f"[INFO] log tail commands sent: {log_tail_count}")
        print(f"[INFO] HardFault count: {self.hardfault_count}")
    
    def run_buffer_test(self, iterations=50):
        print("\n" + "="*60)
        print("BUFFER STRESS TEST")
        print("="*60)
        print(f"[INFO] Testing buffer with large data transfers ({iterations} iterations)")
        
        for i in range(iterations):
            long_cmd = 'a' * 256
            self.command_queue.put((long_cmd, 5))
            
            random_data = ''.join(chr(random.randint(32, 126)) for _ in range(100))
            self.command_queue.put((random_data, 5))
            
            if (i + 1) % 10 == 0:
                print(f"[INFO] Progress: {i+1}/{iterations}")
            
            time.sleep(0.1)
        
        self.command_queue.join()
        
        print("[INFO] Buffer test completed")
    
    def run_reliability_test(self, duration=300):
        print("\n" + "="*60)
        print("RELIABILITY TEST")
        print("="*60)
        print(f"[INFO] Running reliability test for {duration} seconds")
        
        start_time = time.time()
        iteration = 0
        
        while time.time() - start_time < duration and self.running:
            iteration += 1
            
            cmd = random.choice(self.commands)
            self.command_queue.put((cmd, 5))
            
            if iteration % 50 == 0:
                elapsed = time.time() - start_time
                remaining = duration - elapsed
                print(f"[INFO] Iteration: {iteration}, Elapsed: {elapsed:.1f}s, Remaining: {remaining:.1f}s")
            
            time.sleep(random.uniform(0.05, 0.2))
        
        self.command_queue.join()
        
        elapsed = time.time() - start_time
        print(f"[INFO] Reliability test completed in {elapsed:.1f}s")
    
    def print_summary(self):
        print("\n" + "="*60)
        print("TEST SUMMARY")
        print("="*60)
        
        print(f"\n--- Basic Stats ---")
        print(f"Total commands sent: {self.results['total_commands']}")
        print(f"Successful commands: {self.results['successful_commands']}")
        print(f"Failed commands: {self.results['failed_commands']}")
        success_rate = self.results['successful_commands'] / self.results['total_commands'] * 100 if self.results['total_commands'] > 0 else 0
        print(f"Success rate: {success_rate:.1f}%")
        print(f"Prompts received: {self.prompt_count}")
        print(f"HardFault count: {self.hardfault_count}")
        
        print(f"\n--- Buffer Stats ---")
        print(f"Buffer full events: {self.buffer_full_count}")
        
        print(f"\n--- RTT Analysis ---")
        if self.results['rtt_values']:
            min_rtt = min(self.results['rtt_values'])
            max_rtt = max(self.results['rtt_values'])
            avg_rtt = sum(self.results['rtt_values']) / len(self.results['rtt_values'])
            print(f"Min RTT: {min_rtt:.1f}ms")
            print(f"Max RTT: {max_rtt:.1f}ms")
            print(f"Avg RTT: {avg_rtt:.1f}ms")
            if len(self.results['rtt_values']) >= 10:
                p95_rtt = sorted(self.results['rtt_values'])[int(len(self.results['rtt_values']) * 0.95)]
                print(f"95th percentile RTT: {p95_rtt:.1f}ms")
        
        print(f"\n--- Line Stats ---")
        print(f"Total lines received: {len(self.received_lines)}")
        
        print("\n" + "="*60)
        
        if self.hardfault_count > 0:
            print("[FAIL] HardFault detected - system instability!")
            return False
        elif success_rate < 90:
            print("[WARN] Low success rate - check connectivity or timing")
            return False
        else:
            print("[PASS] All tests completed successfully")
            return True
    
    def generate_report(self, output_file='test_report.md'):
        report = []
        report.append("# STM32 CLI Test Report")
        report.append("")
        report.append("## Test Configuration")
        report.append(f"- **Test Date**: {time.strftime('%Y-%m-%d %H:%M:%S')}")
        report.append(f"- **Port**: {self.port}")
        report.append(f"- **Baud Rate**: {self.baud}")
        report.append("")
        
        report.append("## Test Results Summary")
        report.append("")
        report.append("| Metric | Value |")
        report.append("|--------|-------|")
        report.append(f"| Total Commands | {self.results['total_commands']} |")
        report.append(f"| Successful Commands | {self.results['successful_commands']} |")
        report.append(f"| Failed Commands | {self.results['failed_commands']} |")
        success_rate = self.results['successful_commands'] / self.results['total_commands'] * 100 if self.results['total_commands'] > 0 else 0
        report.append(f"| Success Rate | {success_rate:.1f}% |")
        report.append(f"| Prompts Received | {self.prompt_count} |")
        report.append(f"| HardFault Count | {self.hardfault_count} |")
        report.append(f"| Buffer Full Events | {self.buffer_full_count} |")
        report.append(f"| Total Lines Received | {len(self.received_lines)} |")
        report.append("")
        
        report.append("## RTT Analysis")
        report.append("")
        if self.results['rtt_values']:
            min_rtt = min(self.results['rtt_values'])
            max_rtt = max(self.results['rtt_values'])
            avg_rtt = sum(self.results['rtt_values']) / len(self.results['rtt_values'])
            report.append("| Metric | Value |")
            report.append("|--------|-------|")
            report.append(f"| Minimum RTT | {min_rtt:.1f} ms |")
            report.append(f"| Maximum RTT | {max_rtt:.1f} ms |")
            report.append(f"| Average RTT | {avg_rtt:.1f} ms |")
            if len(self.results['rtt_values']) >= 10:
                p95_rtt = sorted(self.results['rtt_values'])[int(len(self.results['rtt_values']) * 0.95)]
                report.append(f"| 95th Percentile RTT | {p95_rtt:.1f} ms |")
        else:
            report.append("No RTT data available")
        report.append("")
        
        report.append("## Test Outcome")
        report.append("")
        if self.hardfault_count > 0:
            report.append("**RESULT: FAIL** - HardFault detected, system instability!")
            report.append("")
            report.append("### Action Required")
            report.append("- Check stack size configuration")
            report.append("- Review memory allocation in CLI commands")
            report.append("- Analyze HardFault handler for crash location")
        elif success_rate < 90:
            report.append("**RESULT: WARN** - Low success rate, check connectivity or timing")
        else:
            report.append("**RESULT: PASS** - All tests completed successfully")
        report.append("")
        
        report.append("## System Health Assessment")
        report.append("")
        if self.hardfault_count == 0 and success_rate >= 99:
            report.append("- ✅ System stable under stress")
            report.append("- ✅ No memory corruption detected")
            report.append("- ✅ CLI commands responsive")
        elif self.hardfault_count == 0 and success_rate >= 90:
            report.append("- ⚠️ System mostly stable but with some timeout issues")
            report.append("- ⚠️ Consider adjusting command timeout settings")
        elif self.hardfault_count == 0 and success_rate >= 80:
            report.append("- ⚠️ System responding but with significant timeout issues")
            report.append("- ⚠️ Increase command delay or timeout values")
        elif self.hardfault_count == 0:
            report.append("- ⚠️ High timeout rate, check communication timing")
            report.append("- ⚠️ Consider increasing inter-command delay")
        else:
            report.append("- ❌ System unstable - HardFault detected")
            report.append("- ❌ Immediate investigation required")
        report.append("")
        
        report.append("## Error Log")
        report.append("")
        if self.results['errors']:
            report.append("| Timestamp | Error Message |")
            report.append("|-----------|---------------|")
            for error in self.results['errors']:
                parts = error.split('] ', 1)
                if len(parts) == 2:
                    timestamp = parts[0][1:]
                    message = parts[1]
                else:
                    timestamp = time.strftime('%Y-%m-%d %H:%M:%S')
                    message = error
                report.append(f"| {timestamp} | {message} |")
        else:
            report.append("No errors recorded")
        report.append("")
        
        with open(output_file, 'w', encoding='utf-8') as f:
            f.write('\n'.join(report))
        
        print(f"[INFO] Report saved to: {output_file}")
    
    def start_test(self, mode='all', duration=300, num_threads=4, num_commands=500, output_file='test_report.md'):
        if not self.connect():
            return False
        
        if not self.check_connectivity():
            self.disconnect()
            return False
        
        self.running = True
        
        reader = threading.Thread(target=self.read_thread)
        reader.daemon = True
        reader.start()
        
        sender = threading.Thread(target=self.sender_thread)
        sender.daemon = True
        sender.start()
        
        print(f"[{time.time():.3f}] [INFO] Starting {mode} test...")
        
        try:
            if mode == 'all' or mode == 'functional':
                self.run_functional_test()
            
            if mode == 'all' or mode == 'functional' or mode == 'top':
                self.run_top_command_test()
            
            if mode == 'all' or mode == 'functional' or mode == 'clr':
                self.run_clr_command_test()
            
            if mode == 'all' or mode == 'logtail':
                self.run_log_tail_test()
            
            if mode == 'all' or mode == 'logtail-perf':
                self.run_log_tail_performance_test()
            
            if mode == 'all' or mode == 'logtail-stress':
                self.run_log_tail_stress_test()
            
            if mode == 'all' or mode == 'stress':
                self.run_stress_test(num_commands=num_commands)
            
            if mode == 'all' or mode == 'concurrent':
                self.run_concurrent_test(num_threads=num_threads, num_commands=num_commands//num_threads)
            
            if mode == 'all' or mode == 'buffer':
                self.run_buffer_test(iterations=50)
            
            if mode == 'reliability':
                self.run_reliability_test(duration=duration)
            
        except KeyboardInterrupt:
            print("[INFO] Test interrupted by user")
        
        self.running = False
        sender.join(timeout=2)
        reader.join(timeout=2)
        
        success = self.print_summary()
        self.generate_report(output_file)
        self.disconnect()
        return success

class PVDTest:
    def __init__(self, port, baud=115200, timeout=5):
        self.port = port
        self.baud = baud
        self.timeout = timeout
        self.ser = None
        self.received_lines = []
        self.pvd_logs = []
        self.reboot_count = 0
        self.is_rebooting = False

    def check_port_available(self):
        try:
            test_ser = serial.Serial(
                port=self.port,
                baudrate=self.baud,
                parity='N',
                stopbits=1,
                bytesize=8,
                timeout=0.1
            )
            test_ser.close()
            return True
        except serial.SerialException as e:
            print(f"[ERROR] Port {self.port} not available: {e}")
            return False
        except Exception as e:
            print(f"[ERROR] Unexpected error checking port: {e}")
            return False

    def find_process_using_port(self):
        if not HAS_WIN32:
            print("[WARN] win32com not available, cannot check process usage")
            return
        try:
            wmi = win32com.client.GetObject("winmgmts:")
            for process in wmi.InstancesOf("Win32_SerialPort"):
                if self.port.lower() in process.DeviceID.lower():
                    print(f"[INFO] Found serial port: {process.DeviceID}")
                    print(f"[INFO] Description: {process.Description}")
            for process in wmi.InstancesOf("Win32_Process"):
                try:
                    cmdline = process.CommandLine
                    if cmdline and self.port in cmdline:
                        print(f"[WARN] Process {process.Name} (PID {process.ProcessId}) may be using {self.port}")
                except:
                    pass
        except Exception as e:
            print(f"[ERROR] WMI query failed: {e}")

    def connect(self, max_retries=5, retry_delay=2):
        for attempt in range(1, max_retries + 1):
            try:
                if self.ser and self.ser.is_open:
                    self.ser.close()
                self.ser = serial.Serial(
                    port=self.port,
                    baudrate=self.baud,
                    parity='N',
                    stopbits=1,
                    bytesize=8,
                    timeout=0.1
                )
                print(f"[INFO] Connected to {self.port} at {self.baud} baud (attempt {attempt}/{max_retries})")
                return True
            except serial.SerialException as e:
                print(f"[WARN] Connection attempt {attempt}/{max_retries} failed: {e}")
                if attempt < max_retries:
                    print(f"[INFO] Retrying in {retry_delay} seconds...")
                    time.sleep(retry_delay)
            except Exception as e:
                print(f"[ERROR] Unexpected error connecting: {e}")
                return False
        print(f"[ERROR] Failed to connect after {max_retries} attempts")
        self.find_process_using_port()
        print("[INFO] Suggestions:")
        print("  1. Check if another terminal (MobaXterm, PuTTY) is using the port")
        print("  2. Close all serial terminal programs")
        print("  3. Try physically resetting the board")
        print("  4. Check device manager for port conflicts")
        return False

    def disconnect(self):
        if self.ser:
            self.ser.close()
            self.ser = None
            print("[INFO] Disconnected")

    def send_command(self, cmd):
        if not self.ser or not self.ser.is_open:
            print("[ERROR] Not connected")
            return
        self.ser.write((cmd + '\r\n').encode('utf-8'))
        print(f"[SEND] '{cmd}'")
        time.sleep(0.2)

    def read_all(self, duration=3):
        if not self.ser or not self.ser.is_open:
            print("[WARN] Cannot read - not connected")
            return
        start_time = time.time()
        buffer = ""
        while time.time() - start_time < duration:
            try:
                if self.ser.in_waiting > 0:
                    data = self.ser.read(self.ser.in_waiting).decode('utf-8', errors='ignore')
                    buffer += data
            except serial.SerialException as e:
                print(f"[ERROR] Serial read error: {e}")
                break
            time.sleep(0.01)
        for chunk in buffer.split('\n'):
            for line in chunk.split('\r'):
                line = line.strip()
                clean = re.sub(r'\x1B\[[0-9;]*[A-Za-z]', '', line).strip()
                if clean:
                    ts = time.time()
                    self.received_lines.append((ts, clean))
                    self.process_pvd_log(clean)
                    if len(self.received_lines) <= 50 or 'PVD' in clean or 'SFUD' in clean:
                        print(f"[RECV] {clean}")

    def process_pvd_log(self, line):
        pvd_patterns = [
            'PVD SIM',
            'PVD ISR',
            'power failure',
            'flushing logs',
            'system reset',
            'voltage recovered'
        ]
        for pattern in pvd_patterns:
            if pattern in line:
                self.pvd_logs.append((time.time(), line))
                print(f"  >>> [PVD-LOG] {line}")
                break

    def wait_for_prompt(self, timeout=10):
        if not self.ser or not self.ser.is_open:
            print("[WARN] Cannot wait for prompt - not connected")
            return False
        start_time = time.time()
        buffer = ""
        while time.time() - start_time < timeout:
            try:
                if self.ser.in_waiting > 0:
                    data = self.ser.read(self.ser.in_waiting).decode('utf-8', errors='ignore')
                    buffer += data
                    while '\n' in buffer:
                        idx = buffer.find('\n')
                        line = buffer[:idx]
                        buffer = buffer[idx+1:]
                        for sub in line.split('\r'):
                            sub = sub.strip()
                            clean = re.sub(r'\x1B\[[0-9;]*[A-Za-z]', '', sub).strip()
                            if clean:
                                self.received_lines.append((time.time(), clean))
                                self.process_pvd_log(clean)
                                print(f"[RECV] {clean}")
                                if clean == '>' or clean == '> ':
                                    return True
            except serial.SerialException as e:
                print(f"[ERROR] Serial error in wait_for_prompt: {e}")
                return False
            time.sleep(0.01)
        print(f"[WARN] Timeout waiting for prompt ({timeout}s)")
        return False

    def wait_for_reboot(self, max_wait=60):
        print("\n" + "-"*60)
        print("WAITING FOR SYSTEM REBOOT")
        print("-"*60)
        start_time = time.time()
        reboot_detected = False
        disconnect_detected = False
        last_activity = time.time()
        print("[INFO] Waiting for serial activity to stop (indicating reset)...")
        while time.time() - start_time < max_wait:
            try:
                if self.ser and self.ser.is_open:
                    if self.ser.in_waiting > 0:
                        data = self.ser.read(self.ser.in_waiting).decode('utf-8', errors='ignore')
                        for chunk in data.split('\n'):
                            for line in chunk.split('\r'):
                                line = line.strip()
                                if line:
                                    clean = re.sub(r'\x1B\[[0-9;]*[A-Za-z]', '', line).strip()
                                    if clean:
                                        self.received_lines.append((time.time(), clean))
                                        self.process_pvd_log(clean)
                                        print(f"[RECV] {clean}")
                                        if 'system reset' in clean or 'NVIC_SystemReset' in clean:
                                            print("[INFO] Reset command detected!")
                                            disconnect_detected = True
                                            last_activity = time.time()
                                        if 'SFUD' in clean or 'EasyLogger' in clean or 'CLI task started' in clean:
                                            reboot_detected = True
                                            print(f"  >>> [REBOOT] System reboot confirmed: {clean[:60]}")
                    if disconnect_detected and time.time() - last_activity > 2:
                        print("[INFO] No more activity, closing port for reconnection...")
                        self.disconnect()
                        break
                else:
                    break
            except serial.SerialException as e:
                print(f"[INFO] Serial disconnected (expected during reboot): {e}")
                disconnect_detected = True
                break
            elapsed = time.time() - start_time
            if int(elapsed) % 5 == 0 and elapsed > 0:
                print(f"[INFO] Waiting for reboot... ({elapsed:.1f}s / {max_wait}s)")
            time.sleep(0.05)
        if reboot_detected:
            print("[OK] Reboot detected before disconnect")
            return True
        print(f"[INFO] Serial disconnected, waiting for board to restart...")
        time.sleep(3)
        print("[INFO] Attempting to reconnect...")
        if self.connect(max_retries=10, retry_delay=2):
            print("[OK] Reconnected after reboot")
            print("[INFO] Waiting for boot sequence...")
            start_time = time.time()
            while time.time() - start_time < 15:
                try:
                    if self.ser.in_waiting > 0:
                        data = self.ser.read(self.ser.in_waiting).decode('utf-8', errors='ignore')
                        for chunk in data.split('\n'):
                            for line in chunk.split('\r'):
                                line = line.strip()
                                if line:
                                    clean = re.sub(r'\x1B\[[0-9;]*[A-Za-z]', '', line).strip()
                                    if clean:
                                        self.received_lines.append((time.time(), clean))
                                        print(f"[BOOT] {clean}")
                                        if 'SFUD' in clean or 'EasyLogger' in clean:
                                            reboot_detected = True
                                            print(f"  >>> [REBOOT] Boot sequence detected")
                                        if clean == '>' or clean == '> ':
                                            print("[OK] Prompt detected after reboot")
                                            return True
                except serial.SerialException as e:
                    print(f"[ERROR] Error during post-reboot read: {e}")
                    return False
                time.sleep(0.05)
            print("[WARN] Boot sequence not fully detected but connected")
            return True
        else:
            print("[FAIL] Failed to reconnect after reboot")
            return False

    def run_test(self):
        print("\n" + "="*60)
        print("PVD Simulation Test")
        print("="*60)
        print(f"[INFO] Port: {self.port}, Baud: {self.baud}")

        if not self.check_port_available():
            print("[ERROR] Port not available, exiting")
            return False

        if not self.connect():
            return False

        print("\n--- Step 1: Send Enter to trigger prompt ---")
        self.ser.write(b'\r\n')
        if not self.wait_for_prompt(timeout=10):
            print("[ERROR] No prompt received. Check if firmware is running.")
            self.disconnect()
            return False
        print("[OK] Prompt detected")

        print("\n--- Step 2: Send 'pvd status' ---")
        self.send_command('pvd status')
        self.read_all(duration=3)

        print("\n--- Step 3: Wait for prompt ---")
        self.ser.write(b'\r\n')
        self.wait_for_prompt(timeout=5)

        print("\n--- Step 4: Send 'pvd test' ---")
        self.send_command('pvd test')

        print("\n--- Step 5: Capture PVD ISR logs ---")
        start_time = time.time()
        max_time = 20
        reset_seen = False

        while time.time() - start_time < max_time:
            try:
                if self.ser and self.ser.in_waiting > 0:
                    data = self.ser.read(self.ser.in_waiting).decode('utf-8', errors='ignore')
                    for chunk in data.split('\n'):
                        for line in chunk.split('\r'):
                            line = line.strip()
                            clean = re.sub(r'\x1B\[[0-9;]*[A-Za-z]', '', line).strip()
                            if clean:
                                self.received_lines.append((time.time(), clean))
                                self.process_pvd_log(clean)
                                print(f"[RECV] {clean}")
                                if 'system reset' in clean:
                                    reset_seen = True
                                    print("[INFO] Reset command received!")
                if reset_seen:
                    print("[INFO] Waiting for serial disconnection...")
                    time.sleep(1)
                    break
            except serial.SerialException as e:
                print(f"[INFO] Serial exception (expected during reset): {e}")
                break
            time.sleep(0.01)

        print("\n--- Step 6: Wait for system reboot and reconnect ---")
        if not self.wait_for_reboot(max_wait=60):
            print("[ERROR] Reboot and reconnection failed")
            self.disconnect()
            return False

        print("\n--- Step 7: Wait for prompt after reboot ---")
        self.ser.write(b'\r\n')
        if self.wait_for_prompt(timeout=15):
            print("[OK] Prompt detected after reboot")
        else:
            print("[WARN] Prompt not detected, trying again...")
            time.sleep(2)
            self.ser.write(b'\r\n')
            self.wait_for_prompt(timeout=5)

        print("\n--- Step 8: Send 'pvd status' after reboot ---")
        self.send_command('pvd status')
        self.read_all(duration=3)

        self.disconnect()
        return self.print_results()

    def print_results(self):
        print("\n" + "="*60)
        print("PVD TEST RESULTS")
        print("="*60)

        print(f"\nTotal lines received: {len(self.received_lines)}")
        print(f"PVD logs captured: {len(self.pvd_logs)}")

        print("\n--- Expected Log Sequence ---")
        expected_sequence = [
            'PVD SIM',
            'PVD ISR: entered',
            'PVD ISR: retry 1',
            'PVD ISR: retry 2',
            'PVD ISR: retry 3',
            'power failure confirmed',
            'flushing logs',
            'system reset'
        ]

        found_count = 0
        for expected in expected_sequence:
            found = any(expected in log[1] for log in self.pvd_logs)
            status = "[OK]" if found else "[MISSING]"
            print(f"  {status} {expected}")
            if found:
                found_count += 1

        print(f"\nExpected: {len(expected_sequence)} / Found: {found_count}")

        reboot_occurred = any('SFUD' in log[1] or 'EasyLogger' in log[1] for log in self.received_lines)
        print(f"\nReboot detected: {'[YES]' if reboot_occurred else '[NO]'}")

        if found_count >= len(expected_sequence) and reboot_occurred:
            print("\n[PASS] PVD simulation test passed!")
            print("[INFO] Complete flow verified: PVD trigger -> ISR -> Log flush -> Reset -> Reboot")
            return True
        elif found_count >= len(expected_sequence):
            print("\n[PARTIAL] All PVD logs detected but reboot not confirmed")
            return False
        elif found_count > 0:
            print(f"\n[PARTIAL] {found_count}/{len(expected_sequence)} steps detected.")
            return False
        else:
            print("\n[FAIL] No PVD logs detected. Check firmware.")
            return False

    def generate_report(self, output_file='test_report.md'):
        report = []
        report.append("# STM32 PVD Test Report")
        report.append("")
        report.append("## Test Configuration")
        report.append(f"- **Test Date**: {time.strftime('%Y-%m-%d %H:%M:%S')}")
        report.append(f"- **Port**: {self.port}")
        report.append(f"- **Baud Rate**: {self.baud}")
        report.append("")
        
        report.append("## Test Results")
        report.append("")
        
        report.append("### Expected Log Sequence")
        report.append("")
        report.append("| Step | Expected | Status |")
        report.append("|------|----------|--------|")
        
        expected_sequence = [
            ('PVD SIM', 'PVD simulation started'),
            ('PVD ISR: entered', 'ISR entered'),
            ('PVD ISR: retry 1', 'First retry'),
            ('PVD ISR: retry 2', 'Second retry'),
            ('PVD ISR: retry 3', 'Third retry'),
            ('power failure confirmed', 'Power failure confirmed'),
            ('flushing logs', 'Logs being flushed'),
            ('system reset', 'System reset triggered'),
        ]
        
        found_count = 0
        for expected, desc in expected_sequence:
            found = any(expected in log[1] for log in self.pvd_logs)
            status = "✅" if found else "❌"
            if found:
                found_count += 1
            report.append(f"| {desc} | {expected} | {status} |")
        
        report.append("")
        report.append(f"**Found: {found_count}/{len(expected_sequence)}**")
        report.append("")
        
        reboot_occurred = any('SFUD' in log[1] or 'EasyLogger' in log[1] for log in self.received_lines)
        report.append(f"### Reboot Detected: {'✅' if reboot_occurred else '❌'}")
        report.append("")
        
        if found_count >= len(expected_sequence) and reboot_occurred:
            report.append("## Test Outcome")
            report.append("")
            report.append("**RESULT: PASS** - PVD simulation test passed!")
            report.append("")
            report.append("### Summary")
            report.append("- ✅ PVD trigger sequence complete")
            report.append("- ✅ ISR executed with 3 retries")
            report.append("- ✅ Power failure confirmed")
            report.append("- ✅ Logs flushed before reset")
            report.append("- ✅ System reboot successful")
        else:
            report.append("## Test Outcome")
            report.append("")
            report.append("**RESULT: FAIL** - PVD test incomplete")
            report.append("")
        
        report.append("## PVD Logs Captured")
        report.append("")
        for ts, log in self.pvd_logs:
            report.append(f"- {log}")
        
        with open(output_file, 'w', encoding='utf-8') as f:
            f.write('\n'.join(report))
        
        print(f"[INFO] Report saved to: {output_file}")

def main():
    parser = argparse.ArgumentParser(description='STM32 Comprehensive Test Suite')
    parser.add_argument('-p', '--port', default='COM3', help='Serial port (default: COM3)')
    parser.add_argument('-b', '--baud', type=int, default=115200, help='Baud rate (default: 115200)')
    parser.add_argument('-m', '--mode', default='all', 
                        choices=['all', 'functional', 'stress', 'concurrent', 'buffer', 'reliability', 'pvd', 'logtail', 'logtail-perf', 'logtail-stress', 'top', 'clr'],
                        help='Test mode')
    parser.add_argument('-d', '--duration', type=int, default=300, help='Test duration in seconds')
    parser.add_argument('-t', '--threads', type=int, default=4, help='Number of concurrent threads')
    parser.add_argument('-c', '--count', type=int, default=500, help='Number of commands')
    parser.add_argument('-o', '--output', default=os.path.join(os.path.dirname(__file__), 'test_report.md'), 
                        help='Output report file')
    
    args = parser.parse_args()
    
    print(f"\nTest Configuration:")
    print(f"  Port: {args.port}")
    print(f"  Baud: {args.baud}")
    print(f"  Mode: {args.mode}")
    print(f"  Duration: {args.duration}s")
    print(f"  Threads: {args.threads}")
    print(f"  Commands: {args.count}")
    print(f"  Output: {args.output}")
    
    if args.mode == 'pvd':
        tester = PVDTest(args.port, args.baud, args.duration)
        success = tester.run_test()
        tester.generate_report(args.output)
        sys.exit(0 if success else 1)
    else:
        tester = CLITestSuite(port=args.port, baud=args.baud)
        success = tester.start_test(
            mode=args.mode,
            duration=args.duration,
            num_threads=args.threads,
            num_commands=args.count,
            output_file=args.output
        )
        sys.exit(0 if success else 1)

if __name__ == "__main__":
    main()