/**
  ******************************************************************************
  * @file    lwip_test_cmds.h
  * @brief   LwIP 网络测试用例 CLI 命令模块头文件
  *
  * 提供 FreeRTOS+CLI 命令：ifconfig / arp / route / ping / tcp_test /
  * udp_test / lwip_test / icmp_diag，用于验证 LwIP 协议栈的接口状态、链路
  * 状态、ARP 表、路由信息、ICMP 连通性及 TCP/UDP 收发。
  *
  * @author  lwip_test_author
  * @date    2026-08-12
  * @version V1.2
  *
  * 修改记录：
  * 2026-08-12  V1.0  首次创建
  * 2026-08-14  V1.1  新增 arp/route/ifconfig 命令；arpinfo 更名为 arp；
  *                   netinfo 与 ifconfig 合并为 ifconfig
  * 2026-08-14  V1.2  新增 icmp_diag on|off 命令：ICMP 收发诊断日志开关
  *                   （默认关闭，避免日志刷屏撑满 TX 缓冲导致 CLI 误判）
  ******************************************************************************
  */

#ifndef __LWIP_TEST_CMDS_H__
#define __LWIP_TEST_CMDS_H__

#ifdef __cplusplus
extern "C" {
#endif

/**
  * @brief  注册所有 LwIP 测试命令到 FreeRTOS+CLI
  * @note   需在 CLI 任务启动前调用（freertos.c 的 StartMyTask 中调用）
  * @retval None
  */
void vRegisterLwipTestCommands(void);

#ifdef __cplusplus
}
#endif

#endif /* __LWIP_TEST_CMDS_H__ */
