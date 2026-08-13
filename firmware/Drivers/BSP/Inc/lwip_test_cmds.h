/**
  ******************************************************************************
  * @file    lwip_test_cmds.h
  * @brief   LwIP 网络测试用例 CLI 命令模块头文件
  *
  * 提供 FreeRTOS+CLI 命令：netinfo / ping / tcp_test / udp_test / lwip_test，
  * 用于验证 LwIP 协议栈的接口状态、链路状态、ICMP 连通性及 TCP/UDP 收发。
  *
  * @author  lwip_test_author
  * @date    2026-08-12
  * @version V1.0
  *
  * 修改记录：
  * 2026-08-12  V1.0  首次创建
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
