# STM32 PVD Test Report

## Test Configuration
- **Test Date**: 2026-08-07 10:57:33
- **Port**: COM3
- **Baud Rate**: 115200

## Test Results

### Expected Log Sequence

| Step | Expected | Status |
|------|----------|--------|
| PVD simulation started | PVD SIM | ✅ |
| ISR entered | PVD ISR: entered | ✅ |
| First retry | PVD ISR: retry 1 | ✅ |
| Second retry | PVD ISR: retry 2 | ✅ |
| Third retry | PVD ISR: retry 3 | ✅ |
| Power failure confirmed | power failure confirmed | ✅ |
| Logs being flushed | flushing logs | ✅ |
| System reset triggered | system reset | ✅ |

**Found: 8/8**

### Reboot Detected: ✅

## Test Outcome

**RESULT: PASS** - PVD simulation test passed!

### Summary
- ✅ PVD trigger sequence complete
- ✅ ISR executed with 3 retries
- ✅ Power failure confirmed
- ✅ Logs flushed before reset
- ✅ System reboot successful
## PVD Logs Captured

- PVD SIM: simulating power failure (force mode)
- PVD ISR: entered
- PVD ISR: retry 1
- PVD ISR: retry 2
- PVD ISR: retry 3
- power failure confirmed
- flushing logs
- system reset