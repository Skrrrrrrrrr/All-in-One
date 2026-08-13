# LwIP Network Test Report

## Test Configuration
- **Test Date**: 2026-08-12 16:24:09
- **Serial Port**: COM3 @ 115200
- **Device IP**: 192.168.10.101
- **PC IP**: 192.168.10.100
- **TCP Port / UDP Port**: 8000 / 8001
- **Payload Length**: 256
- **Ping Count**: 4

## Test Results

| Check | Result |
|-------|--------|
| tcp | ✅ |

**RESULT: PASS** (1/1)

## Notes

- netinfo: 依赖 netif_default 与 DP83848 PHY 驱动
- ping: 依赖 LWIP_RAW=1（lwipopts.h USER CODE 中使能）
- tcp/udp: PC 端由本脚本提供 echo 服务，失败时检查 Windows 防火墙