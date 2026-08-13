/**
  ******************************************************************************
  * @file    lwip_hooks.h
  * @brief   LwIP 自定义 Hook 函数声明（LWIP_HOOK_FILENAME 机制）
  *
  * LwIP 通过在 opt.h 中定义 LWIP_HOOK_FILENAME 指向的文件来向核心代码注入
  * 钩子函数声明。ip4.c 等核心文件会 `#include LWIP_HOOK_FILENAME`，因此本
  * 文件被包含时 LwIP 的 struct pbuf / struct netif 已完整定义，函数原型
  * 不会像放在 lwipopts.h（先于 LwIP 头文件被包含）中那样产生
  * "struct declared inside parameter list" 作用域问题。
  *
  * 启用方式（lwipopts.h USER CODE 段）：
  *   #define LWIP_HOOK_FILENAME "lwip_hooks.h"
  *   #define LWIP_HOOK_IP4_INPUT(pbuf, input_netif) lwip_hook_ip4_input(pbuf, input_netif)
  ******************************************************************************
  */
#ifndef LWIP_HDR_LWIP_HOOKS_H
#define LWIP_HDR_LWIP_HOOKS_H

#include "lwip/arch.h"

struct pbuf;
struct netif;

/* ip4_input 入口钩子（实现在 Drivers/BSP/Src/lwip_test_cmds.c）：
 * 旁路打印收到的 ICMP Echo 帧源/目的 IP 与 netif 路由信息。
 * 返回 0 不消费报文，协议栈照常处理。 */
int lwip_hook_ip4_input(struct pbuf *p, struct netif *inp);

#endif /* LWIP_HDR_LWIP_HOOKS_H */
