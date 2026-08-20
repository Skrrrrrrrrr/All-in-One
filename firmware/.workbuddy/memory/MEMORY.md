# 项目长期笔记：STM32F407 LwIP 调试固件

## 硬件/协议栈
- MCU: STM32F407ZGT6；ETH RMII + DP83848；LWIP 2.1.2（WITH_RTOS=1）。
- 设备静态 IP 192.168.11.101/24，网关 192.168.11.1，DHCP 关闭。
- FreeRTOS + CMSIS-RTOS V2 + FreeRTOS+CLI（USART1 DMA 串口命令行）。
- 日志通道：`vUartRbPrintf()`（UART 环形缓冲），后台任务日志也走它。
  **关键区分**：`vUartRbPrintf()` 仅写 UART 控制台，**不落 flash 文件**；
  落 flash（`log.txt` 及其轮转 `.0/.1/.2`）的是 elog 通道（见 `elog_port.c`
  `elog_file_write`）。所以 `log tail` / `flash` 命令**只读 elog 落盘内容**
  （boot/PVD 等），串口上看到的 `echo_srv`/`[ECHO_RX]` 等实时输出不会出现在
  `log tail` 里——这是设计如此，不是日志丢失。两者都显示到 UART 控制台。

## 网络调试命令（Drivers/BSP/Src）
- `lwip_test_cmds.c`：ifconfig/arp/route/ping/tcp_test/udp_test/lwip_test/icmp_diag
  （一次性回环测试，跑在 CLI 任务上下文）。
- `echo_srv.c`：echo_tcp_srv/echo_udp_srv/echo_stop/echo_status（独立任务，设备=Server
  监听端口，PC 用 NetAssist 作 Client 发送指令，设备原样回传；
  **务必注意设备角色是 Server 而非 Client**）。

## 串口 CLI 终端显示（提示符处理）
- 两条**独立**异步输出通道：① elog → `elog_port_output()` 自带"输入模式下先清行、
  打印、再 `vSetPromptRefreshNeeded()` 重绘提示符"逻辑；② `vUartRbPrintf()`（后台
  任务/网络诊断统一入口）→ 原先**绕过**该逻辑，导致后台消息糊在 `>` 提示符行。
- 已让 `vUartRbPrintf()` 在 `serial_get_input_mode()==pdTRUE` 时复刻同样逻辑
  （`\x1B[2K\r` 清整行 → 打印 → `vSetPromptRefreshNeeded()`），命令执行期间
  `xInUserInputMode=false` 不触发。**切勿把该逻辑放进 `uart_rb_write()` 本身**，
  否则 elog 经由 `uart_rb_write` 会双重处理（清两次行）。
- **致命坑：清行/打印/换行必须合并成一次 `uart_rb_write` 原子写入。** 三者若分三次
  写，CLI 重绘提示符的 `>`（`vSetPromptRefreshNeeded()` 触发，也是一次独立 `uart_rb_write`）
  会**插在"消息"与"换行"之间**，出现 `.....> [ECHO_RX]...` 糊行。一次写入后，`> `
  只能落在整段之前（被 `\x1B[2K` 清掉）或之后（自然在下一行）。消息末尾始终补 `\r\n`
  以保证提示符回到下一行。
- 关键接口（serial.c / UARTCommandConsole.c 共享全局）：`serial_get_input_mode()`
  读 `xInUserInputMode`，`serial_get_input_len()` 读 `ucInputIndex`，提示符
  `"> "` 两字符；重绘走 `vSetPromptRefreshNeeded()` → CLI 循环 `vRefreshCLIPrompt()`。

## 构建接线（CubeMX Makefile 项目，易踩坑）
- 新增手写 .c 到 `Drivers/BSP/Src/` 时，必须手动改 `Debug/Drivers/BSP/Src/subdir.mk`
  的 C_SRCS/OBJS/C_DEPS/clean 目标各加一行；编译由该文件末尾 `%.o: %.c` 模式规则
  自动覆盖。注意：CubeMX 重新生成代码会覆盖此文件，需重加。
- 本地手动编译工具链 `arm-none-eabi-gcc` 在 `/d/VisualCodePlug/arm-none-eabi/bin/`
  （PATH 内），**不含 `-fcyclomatic-complexity`**，手动编译需去掉该标志。

## FreeRTOS / CMSIS-RTOS V2 使用陷阱
- 用 `osThreadNew()` 创建任务时，**必须先写共享状态（尤其运行标志 running）再创建任务**。
  CMSIS-RTOS V2 下 osThreadNew 可能立即抢占当前任务，新任务若先读到未初始化的
  running=0 会直接退出（现象：server/task "刚启动就 stopped"）。正确顺序：先置
  running=1 与各状态 → osThreadNew → 成功后再回填 task_handle。

## LwIP netconn API 使用陷阱
- **TCP 回传必须用 `netconn_write()`，绝不能用 `netconn_send()`**。`netconn_send()`
  仅用于 UDP/RAW（见 api_lib.c 文档 `@ingroup netconn_udp` "not TCP"），其底层
  `lwip_netconn_do_send` 对 TCP 落入 default 分支直接返回 `ERR_CONN`——每包必失败。
- `netconn_write()` 需要**连续缓冲区**，而 `netconn_recv` 收到的 netbuf 可能是链式
  pbuf，故需先用 `netbuf_copy_partial()` 拷出连续缓冲再分块写。
- UDP 回传用 `netconn_sendto(conn, buf, src_addr, src_port)` 即可（buf 里已是来源）。

## SFUD / SPI Flash 初始化（易踩坑）
- 本设备 flash 是**固定 W25Q128BV**（JEDEC ID 0xEF/0x40/0x18），SFUD 静态
  `flash_chip_table` 已有精确参数（16MB / 256B 页 / 4KB 擦除 0x20）。
- **务必关闭 `SFUD_USING_SFDP`**（`Middlewares/Third_Party/SFUD/inc/sfud_cfg.h`）：
  该芯片 SFDP 探测在 flash reset 后**偶发读到乱码签名**，导致启动路径随机分叉
  （有时走 SFDP、有时报 `Check SFDP signature error` 再回退静态表），被误判为
  "reset 失败"。关掉后 `sfud_init()` 永远走静态表，确定性启动、无错误刷屏。
  `SFUD_USING_FLASH_INFO_TABLE` 必须保持开（回退表依赖它）。

## PVD 掉电检测 / reset 命令（易踩坑）
- `reset` 命令（`Sample-CLI-commands.c` `prvResetCommand`）**必须**在
  `NVIC_SystemReset()` 前 `HAL_PWR_DisablePVD()` + 清 PVDO 标志 + 关 PVD IRQ，
  否则本次**主动软件复位会被 PVD 看门狗误读成掉电**，走 `pvd_poll_handler` →
  `NVIC_SystemReset()`，串口打印 `PVD ISR: entered` / `retry N` 噪声。
- 加固：reset 前再 `g_pvd_trigger_count=0` + 清 PVDO + `EXTI->PR=PR16`，防命令执行前
  瞬间锁存的 PVD 事件被 1ms poll handler 劫持。（`g_pvd_event_flag` 是 pvd_detection.c
  的 static，外部清不了，但 DisablePVD+清 PVDO 后 poll handler 读 PVDO=0 会自 abort。）
