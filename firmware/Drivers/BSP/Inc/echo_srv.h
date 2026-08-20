/**
 ******************************************************************************
 * @file    echo_srv.h
 * @brief   Echo server 调试样例（设备 = Server，PC = Client）
 *
 * @note
 *   设计目标：PC 端（NetAssist）作为 Client 向设备发送指令，设备收到后
 *   把这条指令原样回传（echo）。设备在这里扮演 Server 角色：
 *     - TCP：设备 listen 固定端口，PC 用 NetAssist 的 "TCP Client" 连接设备，
 *            之后 PC 发什么、设备回什么。
 *     - UDP：设备 bind 固定端口，PC 把数据发到该端口，设备回传给 PC 的源端口。
 *
 *   命令（在串口 CLI 中输入）：
 *     echo_tcp_srv [port]   启动 TCP echo server（默认端口 8000）
 *     echo_udp_srv [port]   启动 UDP echo server（默认端口 8001）
 *     echo_stop             停止当前 server
 *     echo_status           查看当前 server 状态与收发字节统计
 *
 *   在 Core/Src/freertos.c 的 StartMyTask 中，于 vRegisterLwipTestCommands()
 *   之后调用 vRegisterEchoSrvCommands() 完成注册。
 ******************************************************************************
 */
#ifndef ECHO_SRV_H
#define ECHO_SRV_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief  注册 echo server 调试命令到 FreeRTOS+CLI。
 */
void vRegisterEchoSrvCommands(void);

#ifdef __cplusplus
}
#endif

#endif /* ECHO_SRV_H */
