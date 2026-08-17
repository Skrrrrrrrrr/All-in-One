# STM32 CLI Test Report

## Test Configuration
- **Test Date**: 2026-08-17 12:27:01
- **Port**: COM3
- **Baud Rate**: 115200

## Test Results Summary

| Metric | Value |
|--------|-------|
| Total Commands | 1244 |
| Successful Commands | 1244 |
| Failed Commands | 0 |
| Success Rate | 100.0% |
| Prompts Received | 1239 |
| HardFault Count | 0 |
| Buffer Full Events | 120 |
| Total Lines Received | 30072 |

## RTT Analysis

| Metric | Value |
|--------|-------|
| Minimum RTT | 15.1 ms |
| Maximum RTT | 10921.5 ms |
| Average RTT | 352.5 ms |
| 95th Percentile RTT | 777.0 ms |

## Test Outcome

**RESULT: PASS** - All tests completed successfully

## System Health Assessment

- ✅ System stable under stress
- ✅ No memory corruption detected
- ✅ CLI commands responsive

## Error Log

| Timestamp | Error Message |
|-----------|---------------|
| 2026-08-17 12:18:58 | Connection attempt 1: SerialException - could not open port 'COM3': PermissionError(13, '拒绝访问。', None, 5) |
| 2026-08-17 12:19:01 | Connection attempt 2: SerialException - could not open port 'COM3': PermissionError(13, '拒绝访问。', None, 5) |
