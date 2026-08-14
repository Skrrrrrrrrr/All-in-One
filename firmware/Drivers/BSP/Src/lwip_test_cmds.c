/**
  ******************************************************************************
  * @file    lwip_test_cmds.c
  * @brief   LwIP 网络测试用例 CLI 命令实现
  *
  * 本模块提供以下 FreeRTOS+CLI 命令，用于验证 LwIP 协议栈：
  *   ifconfig                    - 显示网络接口状态（IP/掩码/网关/MAC/链路）
  *   arp                         - 显示 ARP 表（稳定条目）
  *   route                       - 显示路由信息（默认出口与网卡列表）
  *   ping <ip> [count]           - ICMP 回显测试（基于 netconn RAW）
  *   tcp_test <ip> <port> <len>  - TCP 客户端回环测试（PC 端需运行 echo 服务）
  *   udp_test <ip> <port> <len>  - UDP 回环测试（PC 端需运行 echo 服务）
  *   lwip_test <ip> [...]        - 上述测试的集成执行（一次完成全部验证）
  *
  * 设计说明（为什么这样做）：
  *   1. 统一使用 LwIP netconn（Sequential API）：netconn 内部通过 tcpip 线程
  *      访问协议栈，天然线程安全，可直接运行在 CLI 任务上下文中，无需加锁。
  *   2. ping 依赖 RAW 协议：必须在 lwipopts.h 中启用 LWIP_RAW=1，否则
  *      netconn_new_with_proto_and_callback(NETCONN_RAW,...) 分支不会被编译。
  *   3. 每个网络操作均通过 netconn_set_recvtimeout 设置接收超时：避免对端
  *      无响应时命令永久阻塞 CLI 任务（阻塞操作必须有超时机制）。
  *   4. RAW 收包时 LwIP 交付的是"含 IP 头的完整报文"（recv_raw 不做
  *      payload 偏移），因此解析 ICMP 回复时必须跳过 IP_HLEN 个字节。
  *   5. 输出包含机器可解析的关键字（如 "PING: sent=4 recv=4 ..."），
  *      便于 PC 端 Python 脚本（Scripts/lwip_test.py）自动判定 PASS/FAIL。
  *
  * @author  lwip_test_author
  * @date    2026-08-12
  * @version V1.0
  *
  * 修改记录：
  * 2026-08-12  V1.0  首次创建
  ******************************************************************************
  */

/* Includes ------------------------------------------------------------------*/
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stddef.h>          /* offsetof()：packed 结构成员偏移计算 */

#include "FreeRTOS.h"
#include "task.h"
#include "FreeRTOS_CLI.h"

#include "lwip/opt.h"
#include "lwip/api.h"
#include "lwip/pbuf.h"
#include "lwip/netif.h"
#include "lwip/ip_addr.h"
#include "lwip/ip4_addr.h"
#include "lwip/ip4.h"            /* IP_HLEN */
#include "lwip/def.h"            /* htons() */
#include "lwip/prot/ip.h"        /* IP_PROTO_ICMP */
#include "lwip/prot/icmp.h"      /* struct icmp_echo_hdr / ICMP_ECHO / ICMP_ER */
#include "lwip/prot/ethernet.h"  /* struct eth_hdr / ETH_HLEN / ETHTYPE_IP */
#include "lwip/etharp.h"         /* etharp_get_entry() 遍历 ARP 表 */

#include "ethernetif.h"          /* sys_now() */
#include "dp83848.h"             /* DP83848_GetLinkState */

#include "uart_ringbuf.h"        /* vUartRbPrintf：ICMP 诊断日志输出 */

#include "lwip_test_cmds.h"

/* 以太网 MAC 头长度（默认未启用 ETHARP 支持时 hwaddr_len 由网卡填充） */
#define LWIP_TEST_ETH_HWADDR_LEN    (6u)

/* ICMP 回显负载长度（不含 ICMP 头），用于构造探测报文 */
#define LWIP_TEST_PING_PAYLOAD_LEN  (32u)

/* 默认测试参数 */
#define LWIP_TEST_PING_COUNT        (4u)    /* 默认 ping 次数 */
#define LWIP_TEST_PING_TIMEOUT_MS   (1000)  /* 单次 ping 接收超时 */
#define LWIP_TEST_PING_INTERVAL_MS  (1000)  /* 两次 ping 发送间隔 */
#define LWIP_TEST_TCP_TIMEOUT_MS    (3000)  /* TCP 接收超时 */
#define LWIP_TEST_UDP_TIMEOUT_MS    (2000)  /* UDP 接收超时 */
#define LWIP_TEST_DEFAULT_TCP_PORT  (8000)  /* 默认 TCP echo 端口 */
#define LWIP_TEST_DEFAULT_UDP_PORT  (8001)  /* 默认 UDP echo 端口 */
#define LWIP_TEST_DEFAULT_PAYLOAD   (256)   /* 默认收发负载长度 */
#define LWIP_TEST_MAX_PAYLOAD       (1024)  /* 负载长度上限（受 TCP 发送缓冲限制） */
#define LWIP_TEST_PING_MAX_COUNT    (100)   /* ping 次数上限 */
#define LWIP_TEST_TCP_CONNECT_TIMEOUT_MS (3000) /* TCP 非阻塞 connect 总超时 */

/* 外部符号：DP83848 对象在 ethernetif.c 中定义（全局非 static） */
extern dp83848_Object_t DP83848;

/* 私有用例：测试负载缓冲（静态分配，避免占用 CLI 任务栈） */
static uint8_t s_test_payload[LWIP_TEST_MAX_PAYLOAD];

/* 非阻塞 connect 完成标志：由 LwIP 事件回调（tcpip_thread 上下文）置位，
 * 由 CLI 任务轮询读取。TCP connect 在 LwIP 中是异步完成的，且阻塞式
 * connect 在目标不可达时会重试 SYN 最长约 63s（TCP_SYNMAXRTX=6），
 * 会让 CLI 任务长时间挂死；因此测试命令采用非阻塞 connect + 轮询。 */
static volatile int s_tcp_connect_done = 0;

/**
  * @brief  TCP netconn 事件回调（非阻塞 connect 完成检测）
  * @note   连接建立（lwip_netconn_do_connected）与失败（err_tcp）都会
  *         触发 NETCONN_EVT_SENDPLUS 事件；本回调只负责"置位完成标志"，
  *         成功与否由调用方随后通过 netconn_getaddr 查询 TCP 状态确认。
  * @param  conn 触发事件的 netconn（未使用）
  * @param  evt  事件类型
  * @param  len  相关长度（未使用）
  */
static void prvTcpEventCallback(struct netconn *conn, enum netconn_evt evt, uint16_t len)
{
    (void)conn;
    (void)len;
    if (evt == NETCONN_EVT_SENDPLUS) {
        s_tcp_connect_done = 1;
    }
}

/*----------------------------------------------------------------------------*/
/* 辅助函数                                                                    */
/*----------------------------------------------------------------------------*/

/**
  * @brief  将格式化文本追加到输出缓冲（带边界保护）
  * @param  buf  目标缓冲
  * @param  cap  缓冲容量
  * @param  used 当前已用长度
  * @param  fmt  格式化字符串
  * @retval 追加后的已用长度
  */
static size_t prvAppend(char *buf, size_t cap, size_t used, const char *fmt, ...)
{
    va_list ap;
    int n;

    /* 参数有效性检查：防止越界写 */
    if ((buf == NULL) || (cap == 0u) || (used >= cap - 1u) || (fmt == NULL)) {
        return used;
    }

    va_start(ap, fmt);
    n = vsnprintf(buf + used, cap - used, fmt, ap);
    va_end(ap);

    if (n < 0) {
        return used;
    }
    if (used + (size_t)n > cap - 1u) {
        return cap - 1u;
    }
    return used + (size_t)n;
}

/**
  * @brief  解析点分十进制 IPv4 地址
  * @param  ipstr 字符串（如 "192.168.10.100"）
  * @param  dst   输出的 ip_addr_t
  * @retval ERR_OK 成功；ERR_ARG/ERR_VAL 参数非法
  */
static err_t prvParseTarget(const char *ipstr, ip_addr_t *dst)
{
    if ((ipstr == NULL) || (dst == NULL)) {
        return ERR_ARG;
    }
    /* ipaddr_aton 在仅 IPv4 构建下映射为 ip4addr_aton，返回 1 表示成功 */
    if (ipaddr_aton(ipstr, dst) != 1) {
        return ERR_VAL;
    }
    return ERR_OK;
}

/**
  * @brief  用确定性模式填充负载缓冲（便于固件与 PC 脚本两端校验）
  * @param  buf 目标缓冲
  * @param  len 长度
  */
static void prvFillPayload(uint8_t *buf, uint16_t len)
{
    uint16_t i;

    if (buf == NULL) {
        return;
    }
    for (i = 0; i < len; i++) {
        buf[i] = (uint8_t)(((uint16_t)i * 7u + 3u) & 0xFFu);
    }
}

/**
  * @brief  将 LwIP err_t 转换为短字符串（用于结果输出）
  */
static const char *prvErrToStr(err_t err)
{
    switch (err) {
        case ERR_OK:          return "OK";
        case ERR_MEM:         return "MEM";
        case ERR_BUF:         return "BUF";
        case ERR_TIMEOUT:     return "TIMEOUT";
        case ERR_RTE:         return "RTE";
        case ERR_INPROGRESS:  return "INPROGRESS";
        case ERR_VAL:         return "VAL";
        case ERR_WOULDBLOCK:  return "WOULDBLOCK";
        case ERR_USE:         return "USE";
        case ERR_ALREADY:     return "ALREADY";
        case ERR_ISCONN:      return "ISCONN";
        case ERR_CONN:        return "CONN";
        case ERR_IF:          return "IF";
        case ERR_ABRT:        return "ABRT";
        case ERR_RST:         return "RST";
        case ERR_CLSD:        return "CLSD";
        case ERR_ARG:         return "ARG";
        default:              return "UNKNOWN";
    }
}

/*----------------------------------------------------------------------------*/
/* ifconfig：网络接口状态                                                     */
/*----------------------------------------------------------------------------*/

/**
  * @brief  输出网络接口状态信息（含 PHY 链路状态）
  * @param  ok 输出：接口存在且链路已建立则置 1，否则置 0
  * @retval 追加后的缓冲已用长度
  */
static size_t prvDoIfconfig(char *buf, size_t cap, size_t used, int *ok)
{
    struct netif *n = netif_default;
    char ip_s[16] = "0.0.0.0";
    char mask_s[16] = "0.0.0.0";
    char gw_s[16] = "0.0.0.0";
    int32_t phy = 0;
    const char *link_str = "DOWN";
    const char *speed_str = "-";
    uint8_t i;

    if (ok != NULL) {
        *ok = 0;
    }

    if (n == NULL) {
        used = prvAppend(buf, cap, used, "NETIF: NONE (netif not initialized)\r\n");
        return used;
    }

    /* ip4addr_ntoa 使用静态缓冲，多次调用返回同一指针，必须先拷贝到局部数组 */
    strncpy(ip_s, ip4addr_ntoa(netif_ip4_addr(n)), sizeof(ip_s) - 1u);
    strncpy(mask_s, ip4addr_ntoa(netif_ip4_netmask(n)), sizeof(mask_s) - 1u);
    strncpy(gw_s, ip4addr_ntoa(netif_ip4_gw(n)), sizeof(gw_s) - 1u);

    used = prvAppend(buf, cap, used,
                     "NETIF: UP=%s LINK=%s MTU=%u\r\n",
                     netif_is_up(n) ? "UP" : "DOWN",
                     netif_is_link_up(n) ? "UP" : "DOWN",
                     (unsigned)n->mtu);
    used = prvAppend(buf, cap, used,
                     "NETIF: IP=%s MASK=%s GW=%s MAC=", ip_s, mask_s, gw_s);
    for (i = 0; (i < LWIP_TEST_ETH_HWADDR_LEN) && (i < n->hwaddr_len); i++) {
        used = prvAppend(buf, cap, used, "%s%02X", (i == 0u) ? "" : ":", (unsigned)n->hwaddr[i]);
    }
    used = prvAppend(buf, cap, used, "\r\n");

    /* PHY 链路状态：DP83848_GetLinkState 通过 MDIO 读取 BSR 寄存器。
     * 防御：若 IO 尚未注册（ETH 未初始化），直接跳过，避免解引用 NULL 回调。 */
    if ((DP83848.IO.ReadReg != NULL) && (DP83848.IO.GetTick != NULL)) {
        phy = DP83848_GetLinkState(&DP83848);
        switch (phy) {
            case DP83848_STATUS_100MBITS_FULLDUPLEX:
                link_str = "UP"; speed_str = "100M Full-Duplex"; break;
            case DP83848_STATUS_100MBITS_HALFDUPLEX:
                link_str = "UP"; speed_str = "100M Half-Duplex"; break;
            case DP83848_STATUS_10MBITS_FULLDUPLEX:
                link_str = "UP"; speed_str = "10M Full-Duplex"; break;
            case DP83848_STATUS_10MBITS_HALFDUPLEX:
                link_str = "UP"; speed_str = "10M Half-Duplex"; break;
            case DP83848_STATUS_AUTONEGO_NOTDONE:
                link_str = "NEGOTIATING"; speed_str = "-"; break;
            case DP83848_STATUS_LINK_DOWN:
                link_str = "DOWN"; speed_str = "-"; break;
            default:
                link_str = "READ_ERROR"; speed_str = "-"; break;
        }
        used = prvAppend(buf, cap, used, "PHY: link=%s speed=%s\r\n", link_str, speed_str);
    } else {
        used = prvAppend(buf, cap, used, "PHY: not initialized\r\n");
    }

    if (ok != NULL) {
        *ok = ((netif_is_link_up(n) != 0u) && (netif_is_up(n) != 0u)) ? 1 : 0;
    }
    return used;
}

/*----------------------------------------------------------------------------*/
/* ping：ICMP 回显测试                                                          */
/*----------------------------------------------------------------------------*/

/**
  * @brief  对目标主机执行 count 次 ICMP 回显测试
  * @note   RAW netconn 收包时数据从 IP 头开始，解析回显报文需跳过 IP_HLEN。
  * @retval 追加后的缓冲已用长度
  */
static size_t prvDoPing(char *buf, size_t cap, size_t used,
                        const ip_addr_t *target, uint8_t count, int *ok)
{
    struct netconn *conn = NULL;
    struct netbuf *sbuf = NULL;
    struct netbuf *rbuf = NULL;
    static uint8_t tx_buf[sizeof(struct icmp_echo_hdr) + LWIP_TEST_PING_PAYLOAD_LEN];
    uint16_t total_len = (uint16_t)sizeof(tx_buf);
    uint16_t icmp_id = 0;
    uint8_t i;
    uint8_t recv_ok = 0;
    uint32_t min_ms = 0xFFFFFFFFu;
    uint32_t max_ms = 0u;
    uint32_t sum_ms = 0u;

    if (ok != NULL) {
        *ok = 0;
    }
    if ((buf == NULL) || (target == NULL)) {
        return used;
    }
    if (count == 0u) {
        count = LWIP_TEST_PING_COUNT;
    }

    /* RAW 连接需要指定协议号（ICMP），netconn_new(t) 的 proto 固定为 0，不可用 */
    conn = netconn_new_with_proto_and_callback(NETCONN_RAW, IP_PROTO_ICMP, NULL);
    if (conn == NULL) {
        used = prvAppend(buf, cap, used, "PING: netconn create failed\r\n");
        return used;
    }

    /* 关键：设置接收超时，否则无回复时 netconn_recv 会永久阻塞 CLI 任务 */
    netconn_set_recvtimeout(conn, LWIP_TEST_PING_TIMEOUT_MS);

    /* 用系统 Tick 生成 ICMP Identifier，避免与其它 ICMP 流量冲突 */
    icmp_id = (uint16_t)(xTaskGetTickCount() & 0xFFFFu);

    for (i = 0; i < count; i++) {
        struct icmp_echo_hdr *iecho = (struct icmp_echo_hdr *)tx_buf;
        uint8_t *payload = tx_buf + sizeof(struct icmp_echo_hdr);
        void *dptr = NULL;
//        uint16_t dlen = 0;
        uint32_t t0 = 0;
        uint32_t rtt = 0;
        uint8_t attempts = 0;
        uint16_t j;

        memset(tx_buf, 0, sizeof(tx_buf));
        iecho->type = ICMP_ECHO;
        iecho->code = 0;
        iecho->id = htons(icmp_id);
        iecho->seqno = htons(i);
        for (j = 0; j < LWIP_TEST_PING_PAYLOAD_LEN; j++) {
            payload[j] = (uint8_t)(((uint16_t)j * 7u + 3u) & 0xFFu);
        }
        /* ICMP 校验和由 STM32 ETH 硬件在 DMA 发送时计算并插入（TX 描述符
         * ChecksumCtrl = ETH_CHECKSUM_IPHDR_PAYLOAD_INSERT_PHDR_CALC，覆盖
         * TCP/UDP/ICMP）。因此此处必须把校验和字段保持为 0：若软件先填入
         * 校验和，硬件引擎按"整段负载"重新计算，恰得 0x0000 并覆盖之，
         * 导致线上帧校验和无效、Windows 静默丢弃（实测确认）。 */
        iecho->chksum = 0;

        sbuf = netbuf_new();
        if (sbuf == NULL) {
            break;
        }
        dptr = netbuf_alloc(sbuf, total_len);
        if (dptr == NULL) {
            netbuf_delete(sbuf);
            sbuf = NULL;
            break;
        }
        memcpy(dptr, tx_buf, total_len);

        t0 = sys_now();
        {
            /* 诊断：记录 RAW 发送返回码。ERR_MEM=2(内存不足)/ERR_RTE=3(无路由)/
             * ERR_VAL=8(参数错误)。用于区分“帧未发出”(发送失败)与
             * “发出但无应答”(对端未回复)两类故障，避免盲目排查。 */
            err_t send_err = netconn_sendto(conn, sbuf, target, 0);
            if (send_err == ERR_OK) {
            /* 等待匹配的回显回复；seq 不匹配（如上次的迟到回复）时继续等待 */
            for (attempts = 0; attempts < 4u; attempts++) {
                const struct icmp_echo_hdr *rh = NULL;
                const uint8_t *rp = NULL;
                uint16_t rlen = 0;

                rbuf = NULL;
                if (netconn_recv(conn, &rbuf) != ERR_OK) {
                    break;
                }
                if (rbuf == NULL) {
                    break;
                }
                netbuf_data(rbuf, (void **)&rp, &rlen);
                /* RAW 收包从 IP 头开始。IP 头长度不固定（可能带选项），
                 * 必须从 IP 头低 4 位 IHL 字段计算（单位 4 字节），
                 * 不能用固定宏 IP_HLEN(20)，否则带选项头时定位出错。 */
                if (rp != NULL) {
                    uint8_t ihl = (uint8_t)((rp[0] & 0x0Fu) << 2u);

                    if ((rlen >= (uint16_t)ihl + (uint16_t)sizeof(struct icmp_echo_hdr)) &&
                        (rp[ihl] == ICMP_ER)) {
                        rh = (const struct icmp_echo_hdr *)(rp + ihl);
                        if ((rh->id == htons(icmp_id)) && (rh->seqno == htons(i))) {
                            rtt = sys_now() - t0;
                            recv_ok++;
                            if (rtt < min_ms) { min_ms = rtt; }
                            if (rtt > max_ms) { max_ms = rtt; }
                            sum_ms += rtt;
                            netbuf_delete(rbuf);
                            rbuf = NULL;
                            break;
                        }
                    }
                }
                netbuf_delete(rbuf);
                rbuf = NULL;
            }
            } else {
                /* 发送失败：打印错误码，便于定位是内存/路由/参数问题 */
                used = prvAppend(buf, cap, used,
                                 "PING: send err=%d\r\n", (int)send_err);
            }
        }
        if (sbuf != NULL) {
            netbuf_delete(sbuf);
            sbuf = NULL;
        }

        /* 两次 ping 之间固定间隔 1000ms（模拟标准 ping 工具行为）：
         * 逐次探测而非突发，避免连续发包时接收窗口重叠导致 RTT/丢包误判，
         * 也便于 CLI 逐次观察每个回复。count>=4（默认值已兜底），无下溢风险。 */
        if (i < (count - 1u)) {
            vTaskDelay(pdMS_TO_TICKS(LWIP_TEST_PING_INTERVAL_MS));
        }
    }

    netconn_delete(conn);

    /* 诊断：ping 结束后检查目标 IP 的 ARP 解析状态（无需开启全局 LwIP 调试）。
     * 背景：ping recv=0 可能源于两条截然不同的路径——(a) ARP 解析失败，
     * ICMP 帧在 etharp 层排队从未上链路；(b) ARP 解析成功、ICMP 已发出但
     * 对端未回复。用 etharp_find_addr 查询目标是否已有 STABLE 条目可区分二者。
     * 注意：本 LwIP 版本该 API 签名为 (netif, ipaddr, eth_ret, ip_ret)，
     * 返回 STABLE 条目索引(>=0 已解析)；PENDING 条目不视为已解析。 */
    {
        const ip4_addr_t *arp_ip = ip_2_ip4(target);
        struct eth_addr *eth_ret = NULL;
        const ip4_addr_t *ip_ret = NULL;
        ssize_t arp_idx = etharp_find_addr(netif_default, arp_ip, &eth_ret, &ip_ret);

        if (arp_idx >= 0) {
            used = prvAppend(buf, cap, used,
                             "ARP: %s idx=%d resolved -> %02X:%02X:%02X:%02X:%02X:%02X (ip=%s)\r\n",
                             ip4addr_ntoa(arp_ip),
                             (int)arp_idx,
                             (unsigned)eth_ret->addr[0], (unsigned)eth_ret->addr[1],
                             (unsigned)eth_ret->addr[2], (unsigned)eth_ret->addr[3],
                             (unsigned)eth_ret->addr[4], (unsigned)eth_ret->addr[5],
                             (ip_ret != NULL) ? ip4addr_ntoa(ip_ret) : "NULL");
        } else {
            used = prvAppend(buf, cap, used,
                             "ARP: %s NOT resolved (etharp_find_addr=%d)\r\n",
                             ip4addr_ntoa(arp_ip), (int)arp_idx);
        }
    }

    used = prvAppend(buf, cap, used,
                     "PING: sent=%u recv=%u lost=%u min=%lu max=%lu avg=%lu\r\n",
                     (unsigned)count,
                     (unsigned)recv_ok,
                     (unsigned)((uint16_t)count - (uint16_t)recv_ok),
                     (unsigned long)((min_ms == 0xFFFFFFFFu) ? 0u : min_ms),
                     (unsigned long)max_ms,
                     (unsigned long)((recv_ok > 0u) ? (sum_ms / recv_ok) : 0u));

    if (ok != NULL) {
        *ok = (recv_ok == count) ? 1 : 0;
    }
    return used;
}

/*----------------------------------------------------------------------------*/
/* tcp_test：TCP 客户端回环测试                                                 */
/*----------------------------------------------------------------------------*/

/**
  * @brief  TCP 客户端回环测试：连接 -> 发送模式数据 -> 接收回显并逐段校验
  * @note   对端需运行 TCP echo 服务（见 Scripts/lwip_test.py）
  * @retval 追加后的缓冲已用长度
  */
static size_t prvDoTcpTest(char *buf, size_t cap, size_t used,
                           const ip_addr_t *target, uint16_t port, uint16_t len, int *ok)
{
    struct netconn *conn = NULL;
    err_t err = ERR_OK;
    uint32_t t0 = 0;
    uint32_t test_ms = 0;
    uint16_t rx_total = 0;
    uint8_t verify_ok = 1;
    char conn_str[24] = "FAIL";

    if (ok != NULL) {
        *ok = 0;
    }
    if ((buf == NULL) || (target == NULL) || (len == 0u)) {
        return used;
    }
    if (len > LWIP_TEST_MAX_PAYLOAD) {
        len = LWIP_TEST_MAX_PAYLOAD;
    }

    prvFillPayload(s_test_payload, len);

    /* 使用带回调的构造接口：注册非阻塞 connect 完成事件回调。
     * 回调在 tcpip_thread 上下文由 LwIP 触发，仅置位完成标志。 */
    conn = netconn_new_with_proto_and_callback(NETCONN_TCP, 0, prvTcpEventCallback);
    if (conn == NULL) {
        used = prvAppend(buf, cap, used, "TCP_TEST: netconn create failed\r\n");
        return used;
    }
    netconn_set_recvtimeout(conn, LWIP_TEST_TCP_TIMEOUT_MS);

    /* 非阻塞 connect + 轮询：阻塞式 netconn_connect 在目标不可达时会
     * 一直等待 SYN 重试耗尽（TCP_SYNMAXRTX=6，最长约 63s），导致 CLI
     * 任务挂死。改为非阻塞发起连接，再轮询完成事件，总等待有界。 */
    s_tcp_connect_done = 0;
    netconn_set_nonblocking(conn, 1);
    t0 = sys_now();
    err = netconn_connect(conn, target, port);
    if (err == ERR_INPROGRESS) {
        uint32_t waited = 0;

        while ((s_tcp_connect_done == 0) &&
               (waited < LWIP_TEST_TCP_CONNECT_TIMEOUT_MS)) {
            vTaskDelay(pdMS_TO_TICKS(50));
            waited += 50;
        }
        if (s_tcp_connect_done == 0) {
            /* 总超时仍未建立连接：放弃并快速返回，避免长时间无响应 */
            netconn_delete(conn);
            used = prvAppend(buf, cap, used,
                             "TCP_TEST: connect=TIMEOUT(%lu ms)\r\n",
                             (unsigned long)waited);
            return used;
        }
        /* connect 已完成（成功或失败事件都会触发回调），通过查询对端
         * 地址确认当前确实处于 ESTABLISHED，以区分失败回调。 */
        {
            ip_addr_t peer_addr;
            uint16_t peer_port = 0;

            memset(&peer_addr, 0, sizeof(peer_addr));
            err = netconn_getaddr(conn, &peer_addr, &peer_port, 0);
            if (err != ERR_OK) {
                snprintf(conn_str, sizeof(conn_str), "FAIL(%s)", prvErrToStr(err));
                netconn_delete(conn);
                used = prvAppend(buf, cap, used, "TCP_TEST: connect=%s\r\n", conn_str);
                return used;
            }
        }
    } else if (err != ERR_OK) {
        /* 参数错误 / 无路由等立即失败的情况 */
        snprintf(conn_str, sizeof(conn_str), "FAIL(%s)", prvErrToStr(err));
        netconn_delete(conn);
        used = prvAppend(buf, cap, used, "TCP_TEST: connect=%s\r\n", conn_str);
        return used;
    }
    /* 连接成功：恢复阻塞模式，后续 netconn_recv 依靠 recv_timeout 有界等待 */
    netconn_set_nonblocking(conn, 0);
    strncpy(conn_str, "OK", sizeof(conn_str) - 1u);

    err = netconn_write(conn, s_test_payload, (size_t)len, NETCONN_COPY);
    if (err != ERR_OK) {
        netconn_delete(conn);
        used = prvAppend(buf, cap, used,
                         "TCP_TEST: connect=OK write=FAIL(%s)\r\n", prvErrToStr(err));
        return used;
    }

    /* 流式接收：TCP 可能分片到达，按偏移逐段与发送模式比较 */
    while (rx_total < len) {
        struct netbuf *rbuf = NULL;
        const uint8_t *dataptr = NULL;
        uint16_t dlen = 0;

        err = netconn_recv(conn, &rbuf);
        if (err != ERR_OK) {
            break;  /* 超时或对端关闭 */
        }
        if (rbuf == NULL) {
            break;
        }
        netbuf_data(rbuf, (void **)&dataptr, &dlen);
        if ((dataptr != NULL) && (dlen > 0u)) {
            uint16_t cmp_len = dlen;
            if (cmp_len > (uint16_t)(len - rx_total)) {
                cmp_len = (uint16_t)(len - rx_total);
            }
            if (memcmp(dataptr, s_test_payload + rx_total, cmp_len) != 0) {
                verify_ok = 0;
            }
            rx_total = (uint16_t)(rx_total + cmp_len);
        }
        netbuf_delete(rbuf);
    }
    test_ms = sys_now() - t0;

    if (rx_total >= len) {
        netconn_close(conn);
    }
    netconn_delete(conn);

    used = prvAppend(buf, cap, used,
                     "TCP_TEST: connect=%s rtt=%lu ms sent=%u recv=%u verify=%s\r\n",
                     conn_str,
                     (unsigned long)test_ms,
                     (unsigned)len,
                     (unsigned)rx_total,
                     verify_ok ? "OK" : "FAIL");

    if (ok != NULL) {
        *ok = ((rx_total >= len) && (verify_ok != 0u)) ? 1 : 0;
    }
    return used;
}

/*----------------------------------------------------------------------------*/
/* udp_test：UDP 回环测试                                                       */
/*----------------------------------------------------------------------------*/

/**
  * @brief  UDP 回环测试：发送数据报 -> 接收回显并校验
  * @note   对端需运行 UDP echo 服务（见 Scripts/lwip_test.py）
  * @retval 追加后的缓冲已用长度
  */
static size_t prvDoUdpTest(char *buf, size_t cap, size_t used,
                           const ip_addr_t *target, uint16_t port, uint16_t len, int *ok)
{
    struct netconn *conn = NULL;
    struct netbuf *sbuf = NULL;
    struct netbuf *rbuf = NULL;
    err_t err = ERR_OK;
    uint32_t t0 = 0;
    uint32_t test_ms = 0;
    uint16_t rx_total = 0;
    uint8_t verify_ok = 0;
    void *dptr = NULL;
//    uint16_t dlen = 0;

    if (ok != NULL) {
        *ok = 0;
    }
    if ((buf == NULL) || (target == NULL) || (len == 0u)) {
        return used;
    }
    if (len > LWIP_TEST_MAX_PAYLOAD) {
        len = LWIP_TEST_MAX_PAYLOAD;
    }

    prvFillPayload(s_test_payload, len);

    conn = netconn_new(NETCONN_UDP);
    if (conn == NULL) {
        used = prvAppend(buf, cap, used, "UDP_TEST: netconn create failed\r\n");
        return used;
    }
    netconn_set_recvtimeout(conn, LWIP_TEST_UDP_TIMEOUT_MS);

    /* 绑定任意本地端口：netconn_bind 传 NULL 地址等价于 IP4_ADDR_ANY */
    err = netconn_bind(conn, NULL, 0);
    if (err != ERR_OK) {
        netconn_delete(conn);
        used = prvAppend(buf, cap, used,
                         "UDP_TEST: bind=FAIL(%s)\r\n", prvErrToStr(err));
        return used;
    }

    sbuf = netbuf_new();
    if (sbuf != NULL) {
        dptr = netbuf_alloc(sbuf, len);
        if (dptr != NULL) {
            memcpy(dptr, s_test_payload, len);

            t0 = sys_now();
            err = netconn_sendto(conn, sbuf, target, port);
            if (err == ERR_OK) {
                rbuf = NULL;
                err = netconn_recv(conn, &rbuf);
                if ((err == ERR_OK) && (rbuf != NULL)) {
                    const uint8_t *rp = NULL;
                    uint16_t rlen = 0;

                    netbuf_data(rbuf, (void **)&rp, &rlen);
                    if ((rp != NULL) && (rlen == len) &&
                        (memcmp(rp, s_test_payload, len) == 0)) {
                        verify_ok = 1;
                        rx_total = rlen;
                    }
                    netbuf_delete(rbuf);
                }
                test_ms = sys_now() - t0;
            }
        }
        netbuf_delete(sbuf);
        sbuf = NULL;
    }

    netconn_delete(conn);

    used = prvAppend(buf, cap, used,
                     "UDP_TEST: rtt=%lu ms sent=%u recv=%u verify=%s\r\n",
                     (unsigned long)test_ms,
                     (unsigned)len,
                     (unsigned)rx_total,
                     verify_ok ? "OK" : "FAIL");

    if (ok != NULL) {
        *ok = ((rx_total >= len) && (verify_ok != 0u)) ? 1 : 0;
    }
    return used;
}

/*----------------------------------------------------------------------------*/
/* CLI 命令包装                                                               */
/*----------------------------------------------------------------------------*/

/* ifconfig 命令 */
static BaseType_t prvIfconfigCmd(char *pcWriteBuffer, size_t xWriteBufferLen,
                                 const char *pcCommandString)
{
    (void)pcCommandString;

    prvDoIfconfig(pcWriteBuffer, xWriteBufferLen, 0, NULL);
    pcWriteBuffer[xWriteBufferLen - 1u] = '\0';
    return pdFALSE;
}

/* arp 命令：确认 netif 运行时 IP（与配置对比）+ 打印 ARP 表稳定条目。
 * 背景：调试时 etharp 日志反复出现 "ARP request was not for us"，按 lwip 逻辑
 * 这表示收到的 ARP 帧目标 IP != netif 运行时 IP。若设备实际 IP 不是配置的
 * 192.168.10.101，PC 的 ARP 请求会被误判。arp_table 是 etharp.c 内部 static，
 * 外部只能经公共 API etharp_get_entry 遍历（注意：只返回 STABLE 及以上状态，
 * PENDING 条目不可见）。 */
static BaseType_t prvArpCmd(char *pcWriteBuffer, size_t xWriteBufferLen,
                            const char *pcCommandString)
{
    struct netif *n = netif_default;
    ip4_addr_t *ip = NULL;
    struct eth_addr *mac = NULL;
    struct netif *e_netif = NULL;
    char ip_s[16] = "0.0.0.0";
    char mask_s[16] = "0.0.0.0";
    char gw_s[16] = "0.0.0.0";
    size_t used = 0;
    size_t i;
    int entries = 0;

    (void)pcCommandString;

    used = prvAppend(pcWriteBuffer, xWriteBufferLen, used,
                     "\r\n=== ARP Table ===\r\n");

    if (n == NULL) {
        used = prvAppend(pcWriteBuffer, xWriteBufferLen, used,
                         "NETIF: NONE (netif not initialized)\r\n");
        pcWriteBuffer[xWriteBufferLen - 1u] = '\0';
        return pdFALSE;
    }

    /* ip4addr_ntoa 使用静态缓冲，多次调用返回同一指针，必须先拷贝到局部数组 */
    strncpy(ip_s, ip4addr_ntoa(netif_ip4_addr(n)), sizeof(ip_s) - 1u);
    strncpy(mask_s, ip4addr_ntoa(netif_ip4_netmask(n)), sizeof(mask_s) - 1u);
    strncpy(gw_s, ip4addr_ntoa(netif_ip4_gw(n)), sizeof(gw_s) - 1u);

    used = prvAppend(pcWriteBuffer, xWriteBufferLen, used,
                     "NETIF: IP=%s MASK=%s GW=%s UP=%s LINK=%s\r\n",
                     ip_s, mask_s, gw_s,
                     netif_is_up(n) ? "UP" : "DOWN",
                     netif_is_link_up(n) ? "UP" : "DOWN");

    for (i = 0; i < ARP_TABLE_SIZE; i++) {
        if (etharp_get_entry(i, &ip, &e_netif, &mac) == 1) {
            char ipa[16] = "";
            if (ip != NULL) {
                strncpy(ipa, ip4addr_ntoa(ip), sizeof(ipa) - 1u);
            }
            used = prvAppend(pcWriteBuffer, xWriteBufferLen, used,
                             "ARP[%u]: %s -> %02X:%02X:%02X:%02X:%02X:%02X\r\n",
                             (unsigned)i,
                             ipa,
                             (unsigned)mac->addr[0], (unsigned)mac->addr[1],
                             (unsigned)mac->addr[2], (unsigned)mac->addr[3],
                             (unsigned)mac->addr[4], (unsigned)mac->addr[5]);
            entries++;
        }
    }
    used = prvAppend(pcWriteBuffer, xWriteBufferLen, used,
                     "ARP: %d/%u stable entries\r\n",
                     entries, (unsigned)ARP_TABLE_SIZE);

    (void)used;
    pcWriteBuffer[xWriteBufferLen - 1u] = '\0';
    return pdFALSE;
}

/* route 命令：显示路由信息（默认出口 + 全部网卡列表），便于排查
 * "目标不可达 / 报文走错网卡"问题。netif_list 由 lwip/netif.h 声明为 extern，
 * 可遍历当前所有已添加的网卡接口。 */
static BaseType_t prvRouteCmd(char *pcWriteBuffer, size_t xWriteBufferLen,
                              const char *pcCommandString)
{
    struct netif *it = netif_list;
    struct netif *def = netif_default;
    char ip_s[16] = "0.0.0.0";
    char mask_s[16] = "0.0.0.0";
    char gw_s[16] = "0.0.0.0";
    size_t used = 0;
    int count = 0;

    (void)pcCommandString;

    used = prvAppend(pcWriteBuffer, xWriteBufferLen, used,
                     "\r\n=== Routing Table ===\r\n");

    if (def != NULL) {
        /* ip4addr_ntoa 返回静态缓冲，多次调用互相覆盖，必须先逐项拷贝 */
        strncpy(ip_s, ip4addr_ntoa(netif_ip4_addr(def)), sizeof(ip_s) - 1u);
        strncpy(mask_s, ip4addr_ntoa(netif_ip4_netmask(def)), sizeof(mask_s) - 1u);
        strncpy(gw_s, ip4addr_ntoa(netif_ip4_gw(def)), sizeof(gw_s) - 1u);
        used = prvAppend(pcWriteBuffer, xWriteBufferLen, used,
                         "DEFAULT: via %s dev %-2.2s%d IP=%s MASK=%s UP=%s LINK=%s\r\n",
                         gw_s, def->name, def->num, ip_s, mask_s,
                         netif_is_up(def) ? "UP" : "DOWN",
                         netif_is_link_up(def) ? "UP" : "DOWN");
    } else {
        used = prvAppend(pcWriteBuffer, xWriteBufferLen, used,
                         "DEFAULT: none (netif_default == NULL)\r\n");
    }

    for (; it != NULL; it = it->next) {
        const char *is_def = (it == def) ? " *" : "";
        strncpy(ip_s, ip4addr_ntoa(netif_ip4_addr(it)), sizeof(ip_s) - 1u);
        strncpy(mask_s, ip4addr_ntoa(netif_ip4_netmask(it)), sizeof(mask_s) - 1u);
        strncpy(gw_s, ip4addr_ntoa(netif_ip4_gw(it)), sizeof(gw_s) - 1u);
        used = prvAppend(pcWriteBuffer, xWriteBufferLen, used,
                         "NETIF: dev %-2.2s%d%s IP=%s MASK=%s GW=%s UP=%s LINK=%s\r\n",
                         it->name, it->num, is_def, ip_s, mask_s, gw_s,
                         netif_is_up(it) ? "UP" : "DOWN",
                         netif_is_link_up(it) ? "UP" : "DOWN");
        count++;
    }
    used = prvAppend(pcWriteBuffer, xWriteBufferLen, used,
                     "NETIF: %d interface(s)\r\n", count);

    (void)used;
    pcWriteBuffer[xWriteBufferLen - 1u] = '\0';
    return pdFALSE;
}

/* ping 命令：ping <ip> [count] */
static BaseType_t prvPingCmd(char *pcWriteBuffer, size_t xWriteBufferLen,
                             const char *pcCommandString)
{
    const char *pcParam = NULL;
    BaseType_t xParamLen = 0;
    char ip_str[16] = "";
    ip_addr_t target;
    uint8_t count = LWIP_TEST_PING_COUNT;
    size_t used = 0;

    memset(&target, 0, sizeof(target));

    pcParam = FreeRTOS_CLIGetParameter(pcCommandString, 1, &xParamLen);
    if ((pcParam == NULL) || (xParamLen == 0)) {
        snprintf(pcWriteBuffer, xWriteBufferLen,
                 "\r\nping: missing <ip> parameter\r\n");
        return pdFALSE;
    }
    strncpy(ip_str, pcParam, sizeof(ip_str) - 1u);
    if (prvParseTarget(pcParam, &target) != ERR_OK) {
        snprintf(pcWriteBuffer, xWriteBufferLen,
                 "\r\nping: invalid IP '%s'\r\n", ip_str);
        return pdFALSE;
    }

    pcParam = FreeRTOS_CLIGetParameter(pcCommandString, 2, &xParamLen);
    if (pcParam != NULL) {
        long c = strtol(pcParam, NULL, 10);
        if ((c > 0) && (c <= LWIP_TEST_PING_MAX_COUNT)) {
            count = (uint8_t)c;
        }
    }

    used = prvAppend(pcWriteBuffer, xWriteBufferLen, 0,
                     "\r\n=== ping %s (%u) ===\r\n", ip_str, (unsigned)count);
    used = prvDoPing(pcWriteBuffer, xWriteBufferLen, used, &target, count, NULL);
    (void)used;
    pcWriteBuffer[xWriteBufferLen - 1u] = '\0';
    return pdFALSE;
}

/* tcp_test 命令：tcp_test <ip> <port> <len> */
static BaseType_t prvTcpTestCmd(char *pcWriteBuffer, size_t xWriteBufferLen,
                                const char *pcCommandString)
{
    const char *pcParam = NULL;
    BaseType_t xParamLen = 0;
    char ip_str[16] = "";
    ip_addr_t target;
    long port = LWIP_TEST_DEFAULT_TCP_PORT;
    long len = LWIP_TEST_DEFAULT_PAYLOAD;
    size_t used = 0;

    memset(&target, 0, sizeof(target));

    pcParam = FreeRTOS_CLIGetParameter(pcCommandString, 1, &xParamLen);
    if ((pcParam == NULL) || (xParamLen == 0)) {
        snprintf(pcWriteBuffer, xWriteBufferLen,
                 "\r\ntcp_test: missing <ip> parameter\r\n");
        return pdFALSE;
    }
    strncpy(ip_str, pcParam, sizeof(ip_str) - 1u);
    if (prvParseTarget(pcParam, &target) != ERR_OK) {
        snprintf(pcWriteBuffer, xWriteBufferLen,
                 "\r\ntcp_test: invalid IP '%s'\r\n", ip_str);
        return pdFALSE;
    }

    pcParam = FreeRTOS_CLIGetParameter(pcCommandString, 2, &xParamLen);
    if (pcParam != NULL) {
        long p = strtol(pcParam, NULL, 10);
        if ((p > 0) && (p <= 65535)) {
            port = p;
        }
    }
    pcParam = FreeRTOS_CLIGetParameter(pcCommandString, 3, &xParamLen);
    if (pcParam != NULL) {
        long l = strtol(pcParam, NULL, 10);
        if ((l > 0) && (l <= LWIP_TEST_MAX_PAYLOAD)) {
            len = l;
        }
    }

    used = prvAppend(pcWriteBuffer, xWriteBufferLen, 0,
                     "\r\n=== tcp_test %s:%ld len=%ld ===\r\n", ip_str, port, len);
    used = prvDoTcpTest(pcWriteBuffer, xWriteBufferLen, used,
                        &target, (uint16_t)port, (uint16_t)len, NULL);
    (void)used;
    pcWriteBuffer[xWriteBufferLen - 1u] = '\0';
    return pdFALSE;
}

/* udp_test 命令：udp_test <ip> <port> <len> */
static BaseType_t prvUdpTestCmd(char *pcWriteBuffer, size_t xWriteBufferLen,
                                const char *pcCommandString)
{
    const char *pcParam = NULL;
    BaseType_t xParamLen = 0;
    char ip_str[16] = "";
    ip_addr_t target;
    long port = LWIP_TEST_DEFAULT_UDP_PORT;
    long len = LWIP_TEST_DEFAULT_PAYLOAD;
    size_t used = 0;

    memset(&target, 0, sizeof(target));

    pcParam = FreeRTOS_CLIGetParameter(pcCommandString, 1, &xParamLen);
    if ((pcParam == NULL) || (xParamLen == 0)) {
        snprintf(pcWriteBuffer, xWriteBufferLen,
                 "\r\nudp_test: missing <ip> parameter\r\n");
        return pdFALSE;
    }
    strncpy(ip_str, pcParam, sizeof(ip_str) - 1u);
    if (prvParseTarget(pcParam, &target) != ERR_OK) {
        snprintf(pcWriteBuffer, xWriteBufferLen,
                 "\r\nudp_test: invalid IP '%s'\r\n", ip_str);
        return pdFALSE;
    }

    pcParam = FreeRTOS_CLIGetParameter(pcCommandString, 2, &xParamLen);
    if (pcParam != NULL) {
        long p = strtol(pcParam, NULL, 10);
        if ((p > 0) && (p <= 65535)) {
            port = p;
        }
    }
    pcParam = FreeRTOS_CLIGetParameter(pcCommandString, 3, &xParamLen);
    if (pcParam != NULL) {
        long l = strtol(pcParam, NULL, 10);
        if ((l > 0) && (l <= LWIP_TEST_MAX_PAYLOAD)) {
            len = l;
        }
    }

    used = prvAppend(pcWriteBuffer, xWriteBufferLen, 0,
                     "\r\n=== udp_test %s:%ld len=%ld ===\r\n", ip_str, port, len);
    used = prvDoUdpTest(pcWriteBuffer, xWriteBufferLen, used,
                        &target, (uint16_t)port, (uint16_t)len, NULL);
    (void)used;
    pcWriteBuffer[xWriteBufferLen - 1u] = '\0';
    return pdFALSE;
}

/* lwip_test 命令：lwip_test <ip> [tcp_port] [udp_port] [len] */
static BaseType_t prvLwipTestCmd(char *pcWriteBuffer, size_t xWriteBufferLen,
                                 const char *pcCommandString)
{
    const char *pcParam = NULL;
    BaseType_t xParamLen = 0;
    char ip_str[16] = "";
    ip_addr_t target;
    long tcp_port = LWIP_TEST_DEFAULT_TCP_PORT;
    long udp_port = LWIP_TEST_DEFAULT_UDP_PORT;
    long len = LWIP_TEST_DEFAULT_PAYLOAD;
    int net_ok = 0;
    int ping_ok = 0;
    int tcp_ok = 0;
    int udp_ok = 0;
    int all_ok = 0;
    size_t used = 0;

    memset(&target, 0, sizeof(target));

    pcParam = FreeRTOS_CLIGetParameter(pcCommandString, 1, &xParamLen);
    if ((pcParam == NULL) || (xParamLen == 0)) {
        snprintf(pcWriteBuffer, xWriteBufferLen,
                 "\r\nlwip_test <ip> [tcp_port] [udp_port] [len]\r\n"
                 "  Runs: ifconfig + ping + tcp_test + udp_test\r\n");
        return pdFALSE;
    }
    strncpy(ip_str, pcParam, sizeof(ip_str) - 1u);
    if (prvParseTarget(pcParam, &target) != ERR_OK) {
        snprintf(pcWriteBuffer, xWriteBufferLen,
                 "\r\nlwip_test: invalid IP '%s'\r\n", ip_str);
        return pdFALSE;
    }

    pcParam = FreeRTOS_CLIGetParameter(pcCommandString, 2, &xParamLen);
    if (pcParam != NULL) {
        long p = strtol(pcParam, NULL, 10);
        if ((p > 0) && (p <= 65535)) {
            tcp_port = p;
        }
    }
    pcParam = FreeRTOS_CLIGetParameter(pcCommandString, 3, &xParamLen);
    if (pcParam != NULL) {
        long p = strtol(pcParam, NULL, 10);
        if ((p > 0) && (p <= 65535)) {
            udp_port = p;
        }
    }
    pcParam = FreeRTOS_CLIGetParameter(pcCommandString, 4, &xParamLen);
    if (pcParam != NULL) {
        long l = strtol(pcParam, NULL, 10);
        if ((l > 0) && (l <= LWIP_TEST_MAX_PAYLOAD)) {
            len = l;
        }
    }

    used = prvAppend(pcWriteBuffer, xWriteBufferLen, 0,
                     "\r\n=== LwIP Test Suite (target %s, tcp=%ld, udp=%ld, len=%ld) ===\r\n",
                     ip_str, tcp_port, udp_port, len);
    used = prvDoIfconfig(pcWriteBuffer, xWriteBufferLen, used, &net_ok);
    used = prvDoPing(pcWriteBuffer, xWriteBufferLen, used, &target, 3, &ping_ok);
    if ((net_ok != 0) && (ping_ok != 0)) {
        /* 链路与 ICMP 均正常才继续 TCP/UDP 回环测试：
         * 目标不可达时直接跳过，避免 TCP SYN 重试导致的长时间等待 */
        used = prvDoTcpTest(pcWriteBuffer, xWriteBufferLen, used,
                            &target, (uint16_t)tcp_port, (uint16_t)len, &tcp_ok);
        used = prvDoUdpTest(pcWriteBuffer, xWriteBufferLen, used,
                            &target, (uint16_t)udp_port, (uint16_t)len, &udp_ok);
    }

    all_ok = (net_ok != 0) && (ping_ok != 0) && (tcp_ok != 0) && (udp_ok != 0);

    used = prvAppend(pcWriteBuffer, xWriteBufferLen, used,
                     "LWIP_TEST_SUITE: netif=%s ping=%s tcp=%s udp=%s\r\n",
                     (net_ok != 0) ? "PASS" : "FAIL",
                     (ping_ok != 0) ? "PASS" : "FAIL",
                     (tcp_ok != 0) ? "PASS" : "FAIL",
                     (udp_ok != 0) ? "PASS" : "FAIL");
    used = prvAppend(pcWriteBuffer, xWriteBufferLen, used,
                     "RESULT: %s\r\n", all_ok ? "PASS" : "FAIL");
    (void)used;

    pcWriteBuffer[xWriteBufferLen - 1u] = '\0';
    return pdFALSE;
}

/* 命令注册表 ---------------------------------------------------------------*/

/* ifconfig：网络接口状态（IP/掩码/网关/MAC/链路） */
static const CLI_Command_Definition_t xIfconfigCmd = {
    "ifconfig",
    "\r\nifconfig:\r\n Show network interface status (IP/MAC/link)\r\n",
    prvIfconfigCmd,
    0
};

static const CLI_Command_Definition_t xArpCmd = {
    "arp",
    "\r\narp:\r\n Show netif IP + ARP table (stable entries)\r\n",
    prvArpCmd,
    0
};

static const CLI_Command_Definition_t xRouteCmd = {
    "route",
    "\r\nroute:\r\n Show routing info (default via + netif list)\r\n",
    prvRouteCmd,
    0
};

static const CLI_Command_Definition_t xPingCmd = {
    "ping",
    "\r\nping <ip> [count]:\r\n ICMP echo test, default count=4\r\n",
    prvPingCmd,
    -1
};

static const CLI_Command_Definition_t xTcpTestCmd = {
    "tcp_test",
    "\r\ntcp_test <ip> <port> <len>:\r\n TCP echo test (needs echo server)\r\n",
    prvTcpTestCmd,
    -1
};

static const CLI_Command_Definition_t xUdpTestCmd = {
    "udp_test",
    "\r\nudp_test <ip> <port> <len>:\r\n UDP echo test (needs echo server)\r\n",
    prvUdpTestCmd,
    -1
};

static const CLI_Command_Definition_t xLwipTestCmd = {
    "lwip_test",
    "\r\nlwip_test <ip> [tcp_port] [udp_port] [len]:\r\n Full LwIP test suite\r\n",
    prvLwipTestCmd,
    -1
};

/**
  * @brief  注册所有 LwIP 测试命令
  * @retval None
  */
void vRegisterLwipTestCommands(void)
{
    FreeRTOS_CLIRegisterCommand(&xIfconfigCmd);
    FreeRTOS_CLIRegisterCommand(&xArpCmd);
    FreeRTOS_CLIRegisterCommand(&xRouteCmd);
    FreeRTOS_CLIRegisterCommand(&xPingCmd);
    FreeRTOS_CLIRegisterCommand(&xTcpTestCmd);
    FreeRTOS_CLIRegisterCommand(&xUdpTestCmd);
    FreeRTOS_CLIRegisterCommand(&xLwipTestCmd);
}

/*----------------------------------------------------------------------------*/
/* ICMP 收发诊断钩子（LWIP_HOOK_IP4_INPUT + netif->linkoutput 包装）         */
/*----------------------------------------------------------------------------*/
/* 背景：排查"设备 ping PC 收不到回复"。Wireshark 已证明设备 Echo Request 到达
 * PC 网卡但 PC 未回包。以下钩子在设备侧旁路观测 ICMP 收发全过程，与 PC 端
 * 抓包对照，可明确断点发生在哪一侧：
 *   1. lwip_hook_ip4_input：LWIP_HOOK_IP4_INPUT 入口钩子（在 lwipopts.h 启用，
 *      ip4_input 中最先执行，此时 pbuf->payload 指向 IP 头）。打印收到的
 *      ICMP Echo 帧的源/目的 IP 与 netif 路由信息（IP/掩码/网关/默认出口）。
 *      返回 0 不消费报文，icmp_input 照常自动回复 Echo Reply，不改动行为。
 *   2. lwip_linkoutput_wrap：netif->linkoutput 包装（由 lwip.c 在
 *      MX_LWIP_Init 末尾安装）。linkoutput 收到的 pbuf 是完整以太网帧，
 *      打印实际发到链路的 ICMP 帧，确认 Echo Request / Reply 是否真的发出。 */

/**
  * @brief  ip4_input 入口钩子：旁路打印收到的 ICMP Echo 帧与 netif 路由
  * @param  p     收到的 IP 报文（payload 指向 IP 头）
  * @param  inp   接收该报文的网络接口
  * @retval 0     （不消费，协议栈继续正常处理）
  * @note   ip4addr_ntoa 返回的是 LwIP 内部同一个静态缓冲，多次调用会互相
  *         覆盖，必须逐一拷贝到局部数组后再格式化输出。
  */
int lwip_hook_ip4_input(struct pbuf *p, struct netif *inp)
{
    struct ip_hdr *iphdr;
    const u8_t *raw;
    char src_str[16];
    char dst_str[16];
    char ni_str[16];
    char nm_str[16];
    char gw_str[16];

    if ((p == NULL) || (inp == NULL)) {
        return 0;
    }

    iphdr = (struct ip_hdr *)p->payload;
    if (iphdr == NULL) {
        return 0;
    }

    /* 仅观测 ICMP 回显帧（Request=8 / Reply=0），避免其它协议日志刷屏 */
    if (IPH_PROTO(iphdr) == IP_PROTO_ICMP) {
        u8_t ihl = IPH_HL_BYTES(iphdr);
        if (p->len >= ((u16_t)ihl + sizeof(struct icmp_echo_hdr))) {
            const struct icmp_echo_hdr *iehdr =
                (const struct icmp_echo_hdr *)((const u8_t *)iphdr + ihl);

            if ((iehdr->type == ICMP_ECHO) || (iehdr->type == ICMP_ER)) {
                /* struct ip_hdr 是 packed 结构：取成员地址会触发
                 * -Waddress-of-packed-member，且 ip_2_ip4(&iphdr->src) 的
                 * 指针类型在部分版本不可见。改为按字节偏移拷贝到本地
                 * ip4_addr_t（地址值保持网络序，ip4addr_ntoa 会正确处理）。 */
                ip4_addr_t src;
                ip4_addr_t dst;

                raw = (const u8_t *)p->payload;
                memcpy(&src, raw + offsetof(struct ip_hdr, src), sizeof(src));
                memcpy(&dst, raw + offsetof(struct ip_hdr, dest), sizeof(dst));

                /* ip4addr_ntoa 共用同一静态缓冲：逐一拷贝后打印 */
                strncpy(src_str, ip4addr_ntoa(&src), sizeof(src_str) - 1);
                src_str[sizeof(src_str) - 1] = '\0';
                strncpy(dst_str, ip4addr_ntoa(&dst), sizeof(dst_str) - 1);
                dst_str[sizeof(dst_str) - 1] = '\0';
                strncpy(ni_str, ip4addr_ntoa(netif_ip4_addr(inp)), sizeof(ni_str) - 1);
                ni_str[sizeof(ni_str) - 1] = '\0';
                strncpy(nm_str, ip4addr_ntoa(netif_ip4_netmask(inp)), sizeof(nm_str) - 1);
                nm_str[sizeof(nm_str) - 1] = '\0';
                strncpy(gw_str, ip4addr_ntoa(netif_ip4_gw(inp)), sizeof(gw_str) - 1);
                gw_str[sizeof(gw_str) - 1] = '\0';

                vUartRbPrintf(
                    "[ICMP-RX] %s %s -> %s on %c%c%u | "
                    "netif=%s nm=%s gw=%s%s csum=0x%04x\r\n",
                    (iehdr->type == ICMP_ECHO) ? "REQ" : "REPLY",
                    src_str,
                    dst_str,
                    (inp->name[0] != '\0') ? inp->name[0] : '?',
                    (inp->name[1] != '\0') ? inp->name[1] : '?',
                    (unsigned)inp->num,
                    ni_str,
                    nm_str,
                    gw_str,
                    (netif_default == inp) ? " (DEFAULT)" : "",
                    (unsigned)ntohs(iehdr->chksum));
            }
        }
    }

    return 0; /* 不消费：交给 icmp_input 正常处理（自动回复） */
}

/* 保存原始 linkoutput 函数指针（只安装一次，由 lwip.c 调用） */
static err_t (*s_orig_linkoutput)(struct netif *netif, struct pbuf *p) = NULL;

/**
  * @brief  netif->linkoutput 包装：旁路打印设备发到链路的 ICMP 帧
  * @param  netif 发送使用的网络接口
  * @param  p     待发送的完整以太网帧（含 MAC 头）
  * @retval 原 linkoutput 返回值
  * @note   本 LwIP 版本无 ETH_HLEN，以太网头长度使用 SIZEOF_ETH_HDR(=14)。
  */
err_t lwip_linkoutput_wrap(struct netif *netif, struct pbuf *p)
{
    if ((netif != NULL) && (p != NULL)) {
        /* linkoutput 收到的 pbuf 是完整以太网帧（payload 从 MAC 头开始） */
        if (p->tot_len >= (SIZEOF_ETH_HDR + IP_HLEN)) {
            const struct eth_hdr *eh = (const struct eth_hdr *)p->payload;
            if (eh->type == PP_HTONS(ETHTYPE_IP)) {
                const u8_t *ip_raw = (const u8_t *)p->payload + SIZEOF_ETH_HDR;
                const struct ip_hdr *iphdr = (const struct ip_hdr *)ip_raw;
                if (IPH_PROTO(iphdr) == IP_PROTO_ICMP) {
                    u8_t ihl = IPH_HL_BYTES(iphdr);
                    if (p->tot_len >= (SIZEOF_ETH_HDR + (u16_t)ihl + sizeof(struct icmp_echo_hdr))) {
                        const struct icmp_echo_hdr *iehdr =
                            (const struct icmp_echo_hdr *)(ip_raw + ihl);
                        if ((iehdr->type == ICMP_ECHO) || (iehdr->type == ICMP_ER)) {
                            ip4_addr_t src;
                            ip4_addr_t dst;
                            char src_str[16];
                            char dst_str[16];

                            /* 按字节偏移拷贝，避免 packed 成员取地址告警 */
                            memcpy(&src, ip_raw + offsetof(struct ip_hdr, src), sizeof(src));
                            memcpy(&dst, ip_raw + offsetof(struct ip_hdr, dest), sizeof(dst));
                            /* ip4addr_ntoa 共用静态缓冲：逐一拷贝后打印 */
                            strncpy(src_str, ip4addr_ntoa(&src), sizeof(src_str) - 1);
                            src_str[sizeof(src_str) - 1] = '\0';
                            strncpy(dst_str, ip4addr_ntoa(&dst), sizeof(dst_str) - 1);
                            dst_str[sizeof(dst_str) - 1] = '\0';

                            /* ========= ICMP 校验和策略（根因修复） =========
                             * STM32 ETH TX 描述符 ChecksumCtrl 配置为
                             * ETH_CHECKSUM_IPHDR_PAYLOAD_INSERT_PHDR_CALC
                             * （=ETH_DMATXDESC_CIC_TCPUDPICMP_FULL），硬件 DMA
                             * 引擎发送时对 TCP/UDP/ICMP 按整段负载重算校验和并插入。
                             * 因此软件必须把校验和字段保持为 0（prvDoPing 已如此）；
                             * 若软件预先填值，硬件重算后恰得 0x0000 并覆盖之，导致
                             * 线上帧校验和无效、Windows 静默丢弃（Wireshark 实测确认）。
                             * 此处仅旁路观测：打印 csum=0x0000 即符合预期（交硬件插入），
                             * 严禁回写任何值，否则会破坏硬件计算。 */
                            vUartRbPrintf("[ICMP-TX] %s %s -> %s csum=0x%04x (HW-insert)\r\n",
                                (iehdr->type == ICMP_ECHO) ? "REQ" : "REPLY",
                                src_str,
                                dst_str,
                                (unsigned)ntohs(iehdr->chksum));
                        }
                    }
                }
            }
        }
    }

    if (s_orig_linkoutput != NULL) {
        return s_orig_linkoutput(netif, p);
    }
    return ERR_IF;
}

/**
  * @brief  安装 linkoutput 包装（必须在 netif_add 完成、MX_LWIP_Init 末尾调用）
  * @param  netif 待包装的网络接口
  * @note   幂等：仅安装一次，避免重复包装导致递归
  */
void lwip_install_linkoutput_wrap(struct netif *netif)
{
    if ((netif == NULL) || (s_orig_linkoutput != NULL)) {
        return;
    }
    s_orig_linkoutput = netif->linkoutput;
    netif->linkoutput = lwip_linkoutput_wrap;
}
