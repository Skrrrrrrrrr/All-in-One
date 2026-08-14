/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * File Name          : Target/lwipopts.h
  * Description        : This file overrides LwIP stack default configuration
  *                      done in opt.h file.
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */

/* Define to prevent recursive inclusion --------------------------------------*/
#ifndef __LWIPOPTS__H__
#define __LWIPOPTS__H__

#include "main.h"

/*-----------------------------------------------------------------------------*/
/* Current version of LwIP supported by CubeMx: 2.1.2 -*/
/*-----------------------------------------------------------------------------*/

/* Within 'USER CODE' section, code will be kept by default at each generation */
/* USER CODE BEGIN 0 */

/* USER CODE END 0 */

#ifdef __cplusplus
 extern "C" {
#endif

/* STM32CubeMX Specific Parameters (not defined in opt.h) ---------------------*/
/* Parameters set in STM32CubeMX LwIP Configuration GUI -*/
/*----- WITH_RTOS enabled (Since FREERTOS is set) -----*/
#define WITH_RTOS 1
/* Temporary workaround to avoid conflict on errno defined in STM32CubeIDE and lwip sys_arch.c errno */
#undef LWIP_PROVIDE_ERRNO
/*----- CHECKSUM_BY_HARDWARE enabled -----*/
#define CHECKSUM_BY_HARDWARE 1
/*-----------------------------------------------------------------------------*/

/* LwIP Stack Parameters (modified compared to initialization value in opt.h) -*/
/* Parameters set in STM32CubeMX LwIP Configuration GUI -*/
/*----- Value in opt.h for MEM_ALIGNMENT: 1 -----*/
#define MEM_ALIGNMENT 4
/*----- Value in opt.h for LWIP_ETHERNET: LWIP_ARP || PPPOE_SUPPORT -*/
#define LWIP_ETHERNET 1
/*----- Value in opt.h for LWIP_DNS_SECURE: (LWIP_DNS_SECURE_RAND_XID | LWIP_DNS_SECURE_NO_MULTIPLE_OUTSTANDING | LWIP_DNS_SECURE_RAND_SRC_PORT) -*/
#define LWIP_DNS_SECURE 7
/*----- Value in opt.h for TCP_SND_QUEUELEN: (4*TCP_SND_BUF + (TCP_MSS - 1))/TCP_MSS -----*/
#define TCP_SND_QUEUELEN 9
/*----- Value in opt.h for TCP_SNDLOWAT: LWIP_MIN(LWIP_MAX(((TCP_SND_BUF)/2), (2 * TCP_MSS) + 1), (TCP_SND_BUF) - 1) -*/
#define TCP_SNDLOWAT 1071
/*----- Value in opt.h for TCP_SNDQUEUELOWAT: LWIP_MAX(TCP_SND_QUEUELEN)/2, 5) -*/
#define TCP_SNDQUEUELOWAT 5
/*----- Value in opt.h for TCP_WND_UPDATE_THRESHOLD: LWIP_MIN(TCP_WND/4, TCP_MSS*4) -----*/
#define TCP_WND_UPDATE_THRESHOLD 536
/*----- Value in opt.h for LWIP_NETIF_LINK_CALLBACK: 0 -----*/
#define LWIP_NETIF_LINK_CALLBACK 1
/*----- Value in opt.h for TCPIP_THREAD_STACKSIZE: 0 -----*/
#define TCPIP_THREAD_STACKSIZE 1024
/*----- Value in opt.h for TCPIP_THREAD_PRIO: 1 -----*/
#define TCPIP_THREAD_PRIO 24
/*----- Value in opt.h for TCPIP_MBOX_SIZE: 0 -----*/
#define TCPIP_MBOX_SIZE 6
/*----- Value in opt.h for SLIPIF_THREAD_STACKSIZE: 0 -----*/
#define SLIPIF_THREAD_STACKSIZE 1024
/*----- Value in opt.h for SLIPIF_THREAD_PRIO: 1 -----*/
#define SLIPIF_THREAD_PRIO 3
/*----- Value in opt.h for DEFAULT_THREAD_STACKSIZE: 0 -----*/
#define DEFAULT_THREAD_STACKSIZE 1024
/*----- Value in opt.h for DEFAULT_THREAD_PRIO: 1 -----*/
#define DEFAULT_THREAD_PRIO 3
/*----- Value in opt.h for DEFAULT_UDP_RECVMBOX_SIZE: 0 -----*/
#define DEFAULT_UDP_RECVMBOX_SIZE 6
/*----- Value in opt.h for DEFAULT_TCP_RECVMBOX_SIZE: 0 -----*/
#define DEFAULT_TCP_RECVMBOX_SIZE 6
/*----- Value in opt.h for DEFAULT_ACCEPTMBOX_SIZE: 0 -----*/
#define DEFAULT_ACCEPTMBOX_SIZE 6
/*----- Value in opt.h for RECV_BUFSIZE_DEFAULT: INT_MAX -----*/
#define RECV_BUFSIZE_DEFAULT 2000000000
/*----- Value in opt.h for LWIP_STATS: 1 -----*/
#define LWIP_STATS 0
/*----- Value in opt.h for CHECKSUM_GEN_IP: 1 -----*/
#define CHECKSUM_GEN_IP 0
/*----- Value in opt.h for CHECKSUM_GEN_UDP: 1 -----*/
#define CHECKSUM_GEN_UDP 0
/*----- Value in opt.h for CHECKSUM_GEN_TCP: 1 -----*/
#define CHECKSUM_GEN_TCP 0
/*----- Value in opt.h for CHECKSUM_GEN_ICMP: 1 -----*/
#define CHECKSUM_GEN_ICMP 0
/*----- Value in opt.h for CHECKSUM_GEN_ICMP6: 1 -----*/
#define CHECKSUM_GEN_ICMP6 0
/*----- Value in opt.h for CHECKSUM_CHECK_IP: 1 -----*/
#define CHECKSUM_CHECK_IP 0
/*----- Value in opt.h for CHECKSUM_CHECK_UDP: 1 -----*/
#define CHECKSUM_CHECK_UDP 0
/*----- Value in opt.h for CHECKSUM_CHECK_TCP: 1 -----*/
#define CHECKSUM_CHECK_TCP 0
/*----- Value in opt.h for CHECKSUM_CHECK_ICMP: 1 -----*/
#define CHECKSUM_CHECK_ICMP 0
/*----- Value in opt.h for CHECKSUM_CHECK_ICMP6: 1 -----*/
#define CHECKSUM_CHECK_ICMP6 0
/*-----------------------------------------------------------------------------*/
/* USER CODE BEGIN 1 */
/* LwIP 测试命令（lwip_test_cmds.c）中�?? ping 依赖 RAW 协议�??
 * netconn_new_with_proto_and_callback(NETCONN_RAW, IP_PROTO_ICMP)�??
 * opt.h �?? LWIP_RAW 默认值为 0，此处必须显式使能，否则 RAW 分支不参与编译�?? */
#define LWIP_RAW 1

/* LwIP 测试命令依赖接收超时（netconn_set_recvtimeout）：无响应时命令
 * �??在超时后返回而非永久阻塞。api.h 中该接口仅在 LWIP_SO_RCVTIMEO=1 �??
 * 才定义为宏，否则调用成为隐式声明导致链接失败，因此必须启用�?? */
#define LWIP_SO_RCVTIMEO 1

/* RAW netconn（ping 使用）创�?? recvmbox 时需要收件箱容量�??
 * opt.h �?? DEFAULT_RAW_RECVMBOX_SIZE 默认值为 0�??0 会导�??
 * osMessageQueueNew(0,...) 创建失败、netconn_alloc 返回 NULL�??
 * 表现�?? "PING: netconn create failed"。UDP/TCP 收件箱已�?? CubeMX
 * 配置�?? 6，此处与它们保持�??致�?? */
#define DEFAULT_RAW_RECVMBOX_SIZE 6

/* tcpip_thread 栈大小：CubeMX 默认�?? 1024，且�?? osThreadNew 传入�??
 * stack_size 单位是�?�字节�?�（cmsis_os2.c �?? /= sizeof(StackType_t) 转成
 * word），�?? 1024 只有 1KB、上版改�?? 2048 也只�?? 2KB—�?�对承载全部协议
 * 栈处理（eth_input/icmp_input/raw_send/netconn 消息）的 tcpip_thread
 * �?? -O0 下明显不足，深调用链易栈溢出 HardFault（表现为死机）�??
 * 重定义为 4096 字节(1K words)�?? */
#undef  TCPIP_THREAD_STACKSIZE
#define TCPIP_THREAD_STACKSIZE 4096

/* LwIP 堆内存：opt.h 默认 MEM_SIZE=1600 字节，分�?? 1KB �?? PBUF_RAM
 * （tcp_test �?? netconn_write 负载）后�??剩无几，易返�?? ERR_MEM�??
 * 该内存为 mem.c 中的静�?�数组（.bss 段），不占用 FreeRTOS 堆，
 * 此处增大�?? 10KB 以容纳测试负载与 netbuf 分配�?? */
#undef  MEM_SIZE
#define MEM_SIZE (10 * 1024)

/* netbuf 池：opt.h 默认�?? 2 个�?�ping 收到回显回复�?? recv_raw �??从池�??
 * 分配 netbuf，池耗尽会直接丢弃回包（表现�?? ping 丢包），增至 8 个�?? */
#undef  MEMP_NUM_NETBUF
#define MEMP_NUM_NETBUF 8

/* ===== LwIP 调试输出（诊�?? ping 帧未到达对端的问题）=====
 * LWIP_DEBUGF �??终调�?? LWIP_PLATFORM_DIAG（opt.h 默认映射�?? printf）�??
 * 本项�?? printf �?? _write 重定向（freertos.c 中）已被注释掉，直接�?? printf
 * 会丢失全部调试信息；故此处把 LWIP_PLATFORM_DIAG 重定向到串口环形缓冲
 * uart_rb_write，使 tcpip_thread 内的调试消息�?? UART 实时可见�??
 * 注意：lwipopts.h 先于 debug.h 被包含，这里引用 LWIP_DBG_ON 不会�??
 * 定义处展�??，�?�在实际使用处（LWIP_DEBUGF 判定）才展开，因此是安全的�?? */
#include "uart_ringbuf.h"

/* LWIP_PLATFORM_DIAG(x)：x 是�?�完整的带括�?? printf 参数列表】，形如
 * LWIP_PLATFORM_DIAG(("fmt %d", val))。宏必须展开为�?�对带括号列表的调用”，
 * 绝不能把 x 直接传给 vsnprintf(buf,size,x)—�?�x 作为实参展开后是逗号表达式，
 * 编译器只取最后一个整数�?�，会同时引�?? too few arguments /
 * makes pointer from integer / comma expression has no effect 三个错误�??
 * 因此定义可变参数函数 vUartRbPrintf（vsnprintf �?? uart_rb_write）后�??
 * 以�?�vUartRbPrintf x”形式展�??。arch.h 中已�?? #ifndef 保护的默�?? printf
 * 定义，此处必须先 #undef 再重定义，否则某些先包含 arch.h 的编译单元会
 * �?? "LWIP_PLATFORM_DIAG redefined"�?? */
#undef  LWIP_PLATFORM_DIAG
#define LWIP_PLATFORM_DIAG(x) do { vUartRbPrintf x; vUartRbPrintf("\r\n"); } while(0)

/* 重定�?? LwIP 十六进制打印格式宏�??
 * 原因：C99 �?? arch.h �?? PRIx8/PRIx16（newlib 展开�?? "hhx"/"hx"），PRIx8
 * 产生�?? "%02hhx" �?? newlib-nano printf 中处理异常（MAC 显示�?? "0hx"）�??
 * X8_F 使用点不带宽度前�??（如 ethernet.c �?? "%"X8_F），故需自带 "02"�??
 * X16_F 使用点全部自带宽度前�??（如 etharp.c �?? "%02"X16_F、udp.c �??
 * "0x%04"X16_F），若此处再带宽度会拼出 "%0204x"（宽�??204 填充0，超�??0串）�??
 * 因此 X16_F 必须为裸 "x"。两者都避开�?? "hh"/"h" 修饰符，newlib-nano 必支持�?
 * 注意：arch.h �? #ifndef 保护并�? 默认值，但若 arch.h 已先于本文件被包�?
 * （不同翻译单元的 include 顺序不同），直接 #define 会触�? "redefined"�?
 * 因此�? #undef 再定义（�? LWIP_PLATFORM_DIAG 同�?处理策略）�? */
#undef  X8_F
#define X8_F  "02x"
#undef  X16_F
#define X16_F "x"

/* LwIP 调试输出：当前�?�全部关闭�?��??
 * 原因：PC 监测脚本持续�?? ARP 请求（目标非设备），ETHARP_DEBUG 日志经串�??
 * 刷屏干扰观察。需要重新定位网络问题时，将 LWIP_DEBUG �?? 1 并打�??对应组件�??
 * 注：X8_F/X16_F/LWIP_PLATFORM_DIAG 重定义保留，方便直接恢复调试�?? */
#define LWIP_DEBUG 0
#define LWIP_DBG_TYPES_ON LWIP_DBG_OFF
#define RAW_DEBUG          LWIP_DBG_OFF
#define IP_DEBUG           LWIP_DBG_OFF
#define ETHARP_DEBUG       LWIP_DBG_OFF

/* 启用 ip4_input 入口钩子（实现在 lwip_test_cmds.c）：
 * 用于诊断"设备 ping PC 收不到回�?"—�?�在 ip4_input �?先执行，旁路打印
 * 收到�? ICMP Echo 帧源/目的 IP �? netif 路由信息（IP/掩码/网关/默认出口）�??
 * 返回 0 不消费报文，icmp_input 照常自动回复，不影响协议栈行为�??
 * 钩子函数声明�? LWIP_HOOK_FILENAME 机制注入 ip4.c 等核心文�?
 * （见 Drivers/BSP/Inc/lwip_hooks.h）：不能放在 lwipopts.h 中声明，
 * 因为本文件先�? LwIP 头文件被包含，struct pbuf/netif 尚未定义�?
 * 会产�? "declared inside parameter list" 作用域问题与类型冲突�? */
#define LWIP_HOOK_FILENAME "lwip_hooks.h"
#define LWIP_HOOK_IP4_INPUT(pbuf, input_netif) lwip_hook_ip4_input(pbuf, input_netif)
/* USER CODE END 1 */

#ifdef __cplusplus
}
#endif
#endif /*__LWIPOPTS__H__ */
