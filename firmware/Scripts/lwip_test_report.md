# LwIP Network Test Report

## Test Configuration
- **Test Date**: 2026-08-14 18:47:34
- **Serial Port**: COM3 @ 115200
- **Device IP**: 192.168.11.101
- **PC IP**: 192.168.11.100
- **TCP Port / UDP Port**: 8000 / 8001
- **Payload Length**: 256
- **Ping Count**: 4

## Test Results

| Check | Result |
|-------|--------|
| ifconfig | ✅ |
| arp | ✅ |
| route | ✅ |
| suite | ✅ |

**RESULT: PASS** (4/4)

## Notes

- ifconfig: 依赖 netif_default 与 DP83848 PHY 驱动
- arp: 依赖 LWIP_ARP；etharp_get_entry 仅返回 STABLE 及以上条目
- route: 遍历 netif_list，依赖 netif_default
- ping: 依赖 LWIP_RAW=1（lwipopts.h USER CODE 中使能）
- tcp/udp: PC 端由本脚本提供 echo 服务，失败时检查 Windows 防火墙