# STM32 PVD Test Report

## Test Configuration
- **Test Date**: 2026-08-07 14:13:35
- **Port**: COM3
- **Baud Rate**: 115200

## Test Results

### Expected Log Sequence

| Step | Expected | Status |
|------|----------|--------|
| PVD simulation started | PVD SIM | ❌ |
| ISR entered | PVD ISR: entered | ❌ |
| First retry | PVD ISR: retry 1 | ❌ |
| Second retry | PVD ISR: retry 2 | ❌ |
| Third retry | PVD ISR: retry 3 | ❌ |
| Power failure confirmed | power failure confirmed | ❌ |
| Logs being flushed | flushing logs | ❌ |
| System reset triggered | system reset | ❌ |

**Found: 0/8**

### Reboot Detected: ❌

## Test Outcome

**RESULT: FAIL** - PVD test incomplete

## PVD Logs Captured
