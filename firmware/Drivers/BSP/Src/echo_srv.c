/**
 ******************************************************************************
 * @file    echo_srv.c
 * @brief   Echo server 调试样例（设备 = Server，PC = Client）
 *
 *   设备作为 TCP/UDP Server 监听端口。PC 端（NetAssist）作为 Client 发送
 *   "指令"到设备，设备把收到的数据原样回传（echo），并在串口打印收到的内容，
 *   方便用 NetAssist 做交互式链路调试。
 *
 *   与项目已有的 tcp_test/udp_test（设备主动连 PC 的一次性回环测试）不同，
 *   本样例是"设备被动监听、收到即回传"的持续 server，角色为设备 = Server。
 ******************************************************************************
 */
#include "echo_srv.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "FreeRTOS.h"
#include "task.h"
#include "cmsis_os2.h"
#include "FreeRTOS_CLI.h"

#include "lwip/api.h"
#include "lwip/ip_addr.h"
#include "lwip/netbuf.h"

#include "uart_ringbuf.h"

/* ============================ 配置 ============================ */
#define ECHO_SRV_TCP_DEFAULT_PORT   (8000U)
#define ECHO_SRV_UDP_DEFAULT_PORT   (8001U)
#define ECHO_SRV_RECV_TIMEOUT_MS    (500U)   /* 接收超时：既防永久阻塞，也用于轮询停止标志 */
#define ECHO_SRV_TASK_STACK         (4096U)  /* server 任务栈（字节） */
#define ECHO_SRV_TASK_PRIO          (osPriorityNormal)
#define ECHO_SRV_RX_DUMP_MAX        (64U)    /* 串口打印接收内容的最大字节数 */
#define ECHO_SRV_TX_CHUNK           (256U)   /* TCP 回传分块大小（netconn_write 需连续缓冲） */

/* ============================ 状态 ============================ */
typedef enum {
    ECHO_SRV_MODE_NONE = 0,
    ECHO_SRV_MODE_TCP,
    ECHO_SRV_MODE_UDP
} EchoSrvMode_t;

typedef struct {
    EchoSrvMode_t mode;
    uint16_t      local_port;
    uint8_t       running;       /* 由停止命令置 0，server 任务轮询后自行退出 */
    osThreadId_t  task_handle;
    uint32_t      rx_bytes;
    uint32_t      tx_bytes;
    uint32_t      pkt_count;
    uint32_t      conn_count;    /* TCP：累计接受的连接数 */
} EchoSrvState_t;

static EchoSrvState_t s_state = {0};

/* ============================ 内部函数 ============================ */

/**
 * @brief  把接收到的内容以可打印形式打印到串口（不可打印字符显示为 '.'），
 *         便于用 NetAssist 发送指令时观察设备到底收到了什么。
 */
static void prvDumpRx(const uint8_t *data, uint16_t len)
{
    char line[ECHO_SRV_RX_DUMP_MAX * 2 + 24];
    size_t off = 0;
    uint16_t n = (len < ECHO_SRV_RX_DUMP_MAX) ? len : (uint16_t)ECHO_SRV_RX_DUMP_MAX;

    for (uint16_t i = 0; i < n; i++) {
        uint8_t c = data[i];
        if (c >= 0x20 && c < 0x7F) {
            off += (size_t)snprintf(line + off, sizeof(line) - off, "%c", (char)c);
        } else {
            off += (size_t)snprintf(line + off, sizeof(line) - off, ".");
        }
    }
    vUartRbPrintf("[ECHO_RX] %u bytes%s: %s\r\n",
                  (unsigned)len,
                  (len > ECHO_SRV_RX_DUMP_MAX) ? " (truncated)" : "",
                  line);
}

/* 从 netbuf 拷贝前若干字节并打印（netbuf 可能跨多个 pbuf） */
static void prvDumpRxFromNetbuf(struct netbuf *buf)
{
    uint16_t total = netbuf_len(buf);
    uint8_t tmp[ECHO_SRV_RX_DUMP_MAX];
    uint16_t cap = (total < sizeof(tmp)) ? total : (uint16_t)sizeof(tmp);
    if (cap > 0) {
        netbuf_copy(buf, tmp, cap);
    }
    prvDumpRx(tmp, cap);
}

/* ---- TCP server 任务：设备监听端口，PC 作为 client 连接后发送指令，设备原样回传 ---- */
static void prvTcpSrvTask(void *argument)
{
    uint16_t port = (uint16_t)((uint32_t)argument);
    struct netconn *listen_conn = NULL;
    struct netconn *client_conn = NULL;
    struct netbuf *buf = NULL;
    err_t err;
    uint8_t running;

    listen_conn = netconn_new(NETCONN_TCP);
    if (listen_conn == NULL) {
        vUartRbPrintf("ECHO_SRV: tcp netconn_new failed\r\n");
        goto cleanup;
    }
    netconn_set_recvtimeout(listen_conn, ECHO_SRV_RECV_TIMEOUT_MS);

    if (netconn_bind(listen_conn, IP_ANY_TYPE, port) != ERR_OK) {
        vUartRbPrintf("ECHO_SRV: tcp bind port %u failed\r\n", (unsigned)port);
        netconn_delete(listen_conn);
        listen_conn = NULL;
        goto cleanup;
    }
    if (netconn_listen(listen_conn) != ERR_OK) {
        vUartRbPrintf("ECHO_SRV: tcp listen failed\r\n");
        netconn_delete(listen_conn);
        listen_conn = NULL;
        goto cleanup;
    }

    vUartRbPrintf("ECHO_SRV: TCP server listening on :%u, waiting for PC client (NetAssist)...\r\n",
                  (unsigned)port);

    for (;;) {
        taskENTER_CRITICAL();
        running = s_state.running;
        taskEXIT_CRITICAL();
        if (!running) {
            break;
        }

        err = netconn_accept(listen_conn, &client_conn);
        if (err == ERR_TIMEOUT) {
            continue;                       /* 超时：重新检查停止标志 */
        } else if (err != ERR_OK) {
            vUartRbPrintf("ECHO_SRV: tcp accept error %d\r\n", (int)err);
            break;
        }

        taskENTER_CRITICAL();
        s_state.conn_count++;
        taskEXIT_CRITICAL();
        vUartRbPrintf("ECHO_SRV: TCP client connected\r\n");

        netconn_set_recvtimeout(client_conn, ECHO_SRV_RECV_TIMEOUT_MS);

        for (;;) {
            taskENTER_CRITICAL();
            running = s_state.running;
            taskEXIT_CRITICAL();
            if (!running) {
                break;
            }

            err = netconn_recv(client_conn, &buf);
            if (err == ERR_TIMEOUT) {
                continue;
            } else if (err != ERR_OK) {
                break;                      /* 连接断开或出错 */
            }
            if (buf == NULL) {
                continue;
            }

            uint16_t len = netbuf_len(buf);
            prvDumpRxFromNetbuf(buf);

            /* 原样回传：TCP 连接必须用 netconn_write()，netconn_send() 仅用于
             * UDP/RAW（用在 TCP 上 lwIP 底层直接返回 ERR_CONN，见 api_msg.c
             * 中 lwip_netconn_do_send 的 default 分支）。收到的 netbuf 可能是
             * 链式 pbuf，netconn_write 需要连续缓冲，故分块拷出再写。 */
            {
                static uint8_t s_tx_chunk[ECHO_SRV_TX_CHUNK];
                uint16_t remaining = len;
                uint16_t offset = 0;
                err_t werr = ERR_OK;

                while (remaining > 0 && werr == ERR_OK) {
                    uint16_t chunk = (remaining < (uint16_t)ECHO_SRV_TX_CHUNK)
                                     ? remaining : (uint16_t)ECHO_SRV_TX_CHUNK;
                    netbuf_copy_partial(buf, s_tx_chunk, chunk, offset);
                    werr = netconn_write(client_conn, s_tx_chunk, chunk, NETCONN_COPY);
                    offset += chunk;
                    remaining -= chunk;
                }

                if (werr == ERR_OK) {
                    taskENTER_CRITICAL();
                    s_state.rx_bytes += len;
                    s_state.tx_bytes += len;
                    s_state.pkt_count++;
                    taskEXIT_CRITICAL();
                } else {
                    vUartRbPrintf("ECHO_SRV: tcp send back failed (%d)\r\n", (int)werr);
                }
            }
            netbuf_delete(buf);
            buf = NULL;
        }

        netconn_close(client_conn);
        netconn_delete(client_conn);
        client_conn = NULL;
        vUartRbPrintf("ECHO_SRV: TCP client disconnected\r\n");
    }

cleanup:
    if (client_conn != NULL) {
        netconn_close(client_conn);
        netconn_delete(client_conn);
    }
    if (listen_conn != NULL) {
        netconn_delete(listen_conn);
    }
    taskENTER_CRITICAL();
    s_state.running = 0;
    s_state.task_handle = NULL;
    taskEXIT_CRITICAL();
    vUartRbPrintf("ECHO_SRV: TCP server stopped\r\n");
    osThreadExit();
}

/* ---- UDP server 任务：设备绑定端口，收到任意发送方的数据后回传给该发送方 ---- */
static void prvUdpSrvTask(void *argument)
{
    uint16_t port = (uint16_t)((uint32_t)argument);
    struct netconn *conn = NULL;
    struct netbuf *buf = NULL;
    err_t err;
    uint8_t running;

    conn = netconn_new(NETCONN_UDP);
    if (conn == NULL) {
        vUartRbPrintf("ECHO_SRV: udp netconn_new failed\r\n");
        goto cleanup;
    }
    netconn_set_recvtimeout(conn, ECHO_SRV_RECV_TIMEOUT_MS);

    if (netconn_bind(conn, IP_ANY_TYPE, port) != ERR_OK) {
        vUartRbPrintf("ECHO_SRV: udp bind port %u failed\r\n", (unsigned)port);
        netconn_delete(conn);
        conn = NULL;
        goto cleanup;
    }

    vUartRbPrintf("ECHO_SRV: UDP server bound on :%u, echoing every sender (NetAssist)...\r\n",
                  (unsigned)port);

    for (;;) {
        taskENTER_CRITICAL();
        running = s_state.running;
        taskEXIT_CRITICAL();
        if (!running) {
            break;
        }

        err = netconn_recv(conn, &buf);
        if (err == ERR_TIMEOUT) {
            continue;
        } else if (err != ERR_OK) {
            vUartRbPrintf("ECHO_SRV: udp recv error %d\r\n", (int)err);
            break;
        }
        if (buf == NULL) {
            continue;
        }

        ip_addr_t *src = netbuf_fromaddr(buf);
        u16_t src_port = netbuf_fromport(buf);
        uint16_t len = netbuf_len(buf);

        prvDumpRxFromNetbuf(buf);

        /* 原样回传：发回数据实际来源的地址与端口 */
        if (netconn_sendto(conn, buf, src, src_port) == ERR_OK) {
            taskENTER_CRITICAL();
            s_state.rx_bytes += len;
            s_state.tx_bytes += len;
            s_state.pkt_count++;
            taskEXIT_CRITICAL();
        } else {
            vUartRbPrintf("ECHO_SRV: udp send back failed\r\n");
        }
        netbuf_delete(buf);
        buf = NULL;
    }

cleanup:
    if (conn != NULL) {
        netconn_delete(conn);
    }
    taskENTER_CRITICAL();
    s_state.running = 0;
    s_state.task_handle = NULL;
    taskEXIT_CRITICAL();
    vUartRbPrintf("ECHO_SRV: UDP server stopped\r\n");
    osThreadExit();
}

/* 启动一个 server（若已有 server 运行则拒绝）。返回 0=成功, 1=创建失败, 2=已有运行 */
static BaseType_t prvStartServer(EchoSrvMode_t mode, uint16_t port)
{
    osThreadId_t h;
    osThreadAttr_t attr;
    const char *name;

    /* 先置运行状态与配置，再创建任务。否则在 CMSIS-RTOS V2 下 osThreadNew
     * 可能立即调度新任务（其优先级高于 CLI），而任务在 running 尚未置 1 前
     * 就因 !running 退出，表现为"刚启动就 stopped"。 */
    taskENTER_CRITICAL();
    if (s_state.running) {
        taskEXIT_CRITICAL();
        return 2;                       /* 已有 server 在运行 */
    }
    s_state.mode = mode;
    s_state.local_port = port;
    s_state.running = 1;
    s_state.task_handle = NULL;
    s_state.rx_bytes = 0;
    s_state.tx_bytes = 0;
    s_state.pkt_count = 0;
    s_state.conn_count = 0;
    taskEXIT_CRITICAL();

    memset(&attr, 0, sizeof(attr));
    name = (mode == ECHO_SRV_MODE_TCP) ? "echo_tcp_srv" : "echo_udp_srv";
    attr.name = name;
    attr.stack_size = ECHO_SRV_TASK_STACK;
    attr.priority = ECHO_SRV_TASK_PRIO;

    h = osThreadNew((mode == ECHO_SRV_MODE_TCP) ? prvTcpSrvTask : prvUdpSrvTask,
                    (void *)(uint32_t)port, &attr);
    if (h == NULL) {
        taskENTER_CRITICAL();
        s_state.running = 0;            /* 创建失败，回滚运行状态 */
        taskEXIT_CRITICAL();
        return 1;                       /* 任务创建失败 */
    }

    taskENTER_CRITICAL();
    s_state.task_handle = h;
    taskEXIT_CRITICAL();
    return 0;
}

/* 停止当前 server：先置停止标志等待优雅退出，超时再强制终止 */
static BaseType_t prvStopServer(void)
{
    osThreadId_t h;

    taskENTER_CRITICAL();
    if (!s_state.running) {
        taskEXIT_CRITICAL();
        return 1;                       /* 本就未运行 */
    }
    s_state.running = 0;
    h = s_state.task_handle;
    taskEXIT_CRITICAL();

    /* 接收超时 500ms，最多等约 1.5s 让任务自行退出 */
    for (int i = 0; i < 6; i++) {
        osDelay(250);
        taskENTER_CRITICAL();
        if (!s_state.running && s_state.task_handle == NULL) {
            taskEXIT_CRITICAL();
            return 0;
        }
        taskEXIT_CRITICAL();
    }

    /* 兜底：强制终止（可能泄漏 netconn，仅调试场景） */
    if (h != NULL) {
        osThreadTerminate(h);
    }
    taskENTER_CRITICAL();
    s_state.running = 0;
    s_state.task_handle = NULL;
    taskEXIT_CRITICAL();
    vUartRbPrintf("ECHO_SRV: forced terminate (netconn may leak)\r\n");
    return 0;
}

/* ============================ CLI 命令回调 ============================ */

static BaseType_t prvTcpSrvCmd(char *pcWriteBuffer, size_t xWriteBufferLen, const char *pcCommandString)
{
    const char *param;
    uint16_t port = (uint16_t)ECHO_SRV_TCP_DEFAULT_PORT;

    param = FreeRTOS_CLIGetParameter(pcCommandString, 1, NULL);
    if (param != NULL) {
        int p = atoi(param);
        if (p > 0 && p <= 65535) {
            port = (uint16_t)p;
        }
    }

    switch (prvStartServer(ECHO_SRV_MODE_TCP, port)) {
        case 0:
            snprintf(pcWriteBuffer, xWriteBufferLen,
                     "\r\nECHO_SRV: TCP server starting on :%u (device=server, PC=client)...\r\n",
                     (unsigned)port);
            break;
        case 2:
            snprintf(pcWriteBuffer, xWriteBufferLen,
                     "\r\nECHO_SRV: a server is already running, stop it first (echo_stop)\r\n");
            break;
        default:
            snprintf(pcWriteBuffer, xWriteBufferLen,
                     "\r\nECHO_SRV: failed to create TCP server task\r\n");
            break;
    }
    return pdFALSE;
}

static BaseType_t prvUdpSrvCmd(char *pcWriteBuffer, size_t xWriteBufferLen, const char *pcCommandString)
{
    const char *param;
    uint16_t port = (uint16_t)ECHO_SRV_UDP_DEFAULT_PORT;

    param = FreeRTOS_CLIGetParameter(pcCommandString, 1, NULL);
    if (param != NULL) {
        int p = atoi(param);
        if (p > 0 && p <= 65535) {
            port = (uint16_t)p;
        }
    }

    switch (prvStartServer(ECHO_SRV_MODE_UDP, port)) {
        case 0:
            snprintf(pcWriteBuffer, xWriteBufferLen,
                     "\r\nECHO_SRV: UDP server starting on :%u (device=server, PC=client)...\r\n",
                     (unsigned)port);
            break;
        case 2:
            snprintf(pcWriteBuffer, xWriteBufferLen,
                     "\r\nECHO_SRV: a server is already running, stop it first (echo_stop)\r\n");
            break;
        default:
            snprintf(pcWriteBuffer, xWriteBufferLen,
                     "\r\nECHO_SRV: failed to create UDP server task\r\n");
            break;
    }
    return pdFALSE;
}

static BaseType_t prvStopCmd(char *pcWriteBuffer, size_t xWriteBufferLen, const char *pcCommandString)
{
    (void)pcCommandString;
    if (prvStopServer() == 1) {
        snprintf(pcWriteBuffer, xWriteBufferLen, "\r\nECHO_SRV: no server running\r\n");
    } else {
        snprintf(pcWriteBuffer, xWriteBufferLen, "\r\nECHO_SRV: stopping...\r\n");
    }
    return pdFALSE;
}

static BaseType_t prvStatusCmd(char *pcWriteBuffer, size_t xWriteBufferLen, const char *pcCommandString)
{
    EchoSrvMode_t m;
    uint16_t p;
    uint8_t running;
    uint32_t rx, tx, pk, cn;

    (void)pcCommandString;

    taskENTER_CRITICAL();
    m = s_state.mode;
    p = s_state.local_port;
    running = s_state.running;
    rx = s_state.rx_bytes;
    tx = s_state.tx_bytes;
    pk = s_state.pkt_count;
    cn = s_state.conn_count;
    taskEXIT_CRITICAL();

    const char *mode_str = (m == ECHO_SRV_MODE_TCP) ? "TCP"
                          : (m == ECHO_SRV_MODE_UDP) ? "UDP" : "NONE";
    snprintf(pcWriteBuffer, xWriteBufferLen,
             "\r\nECHO_SRV status:\r\n"
             " mode    : %s\r\n"
             " port    : %u\r\n"
             " running : %s\r\n"
             " rx/tx   : %lu / %lu bytes\r\n"
             " packets : %lu\r\n"
             " conns   : %lu\r\n",
             mode_str, (unsigned)p, running ? "yes" : "no",
             (unsigned long)rx, (unsigned long)tx,
             (unsigned long)pk, (unsigned long)cn);
    return pdFALSE;
}

/* ============================ 命令注册 ============================ */

static const CLI_Command_Definition_t xTcpSrvCmd = {
    "echo_tcp_srv",
    "echo_tcp_srv [port]:\r\n Start TCP echo server (device=server, PC=client), default 8000\r\n",
    prvTcpSrvCmd,
    -1
};

static const CLI_Command_Definition_t xUdpSrvCmd = {
    "echo_udp_srv",
    "echo_udp_srv [port]:\r\n Start UDP echo server (device=server, PC=client), default 8001\r\n",
    prvUdpSrvCmd,
    -1
};

static const CLI_Command_Definition_t xStopCmd = {
    "echo_stop",
    "echo_stop:\r\n Stop current echo server\r\n",
    prvStopCmd,
    0
};

static const CLI_Command_Definition_t xStatusCmd = {
    "echo_status",
    "echo_status:\r\n Show echo server status and byte counters\r\n",
    prvStatusCmd,
    0
};

void vRegisterEchoSrvCommands(void)
{
    FreeRTOS_CLIRegisterCommand(&xTcpSrvCmd);
    FreeRTOS_CLIRegisterCommand(&xUdpSrvCmd);
    FreeRTOS_CLIRegisterCommand(&xStopCmd);
    FreeRTOS_CLIRegisterCommand(&xStatusCmd);
}
