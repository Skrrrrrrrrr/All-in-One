#include "uart_ringbuf.h"
#include "stm32f4xx_hal.h"
#include "FreeRTOS.h"
#include "task.h"
#include <string.h>
#include <stdarg.h>
#include <stdio.h>
#include "serial.h"   /* serial_get_input_mode / vSetPromptRefreshNeeded：
                        让后台异步输出在 CLI 输入模式下先清提示符行再打印 */

static uint8_t uart_rb[UART_RB_SIZE];
static volatile uint32_t rb_head = 0;
static volatile uint32_t rb_tail = 0;
static volatile uint8_t rb_busy = 0;

static uart_rb_stats_t rb_stats = {0};

static uart_rb_context_t s_context = {
    .huart = NULL,
    .buffer = uart_rb,
    .size = UART_RB_SIZE,
};

int uart_rb_register(UART_HandleTypeDef *huart)
{
    if (huart == NULL || huart->Instance == NULL) {
        return -1;
    }
    s_context.huart = huart;
    return 0;
}

/* 返回串口 TX 通道是否已注册：供依赖方（如 LwIP 初始化守卫）判断
 * uart_rb 输出通道是否可用，避免在通道就绪前触发串口输出或错误初始化。 */
int uart_rb_is_ready(void)
{
    return (s_context.huart != NULL) ? 1 : 0;
}

const uart_rb_context_t *uart_rb_get_context(void)
{
    return &s_context;
}

void uart_rb_init(void)
{
    memset((void*)uart_rb, 0, UART_RB_SIZE);

    if (s_context.huart == NULL || s_context.huart->Instance == NULL) {
        return;
    }

    if (s_context.huart->hdmatx != NULL && s_context.huart->hdmatx->Instance != NULL) {
        HAL_DMA_Abort(s_context.huart->hdmatx);
        while (HAL_DMA_GetState(s_context.huart->hdmatx) != HAL_DMA_STATE_READY) {}
    }

    HAL_UART_AbortTransmit(s_context.huart);
    while (s_context.huart->gState != HAL_UART_STATE_READY) {}

    rb_head = 0;
    rb_tail = 0;
    rb_busy = 0;
    rb_stats.total_written = 0;
    rb_stats.total_dropped = 0;
    rb_stats.dma_tx_count = 0;
    rb_stats.buffer_full_count = 0;
    rb_stats.current_used = 0;
}

static inline uint32_t rb_count(void)
{
    uint32_t h = rb_head;
    uint32_t t = rb_tail;
	return h >= t ? (h - t) : ( UART_RB_SIZE - t + h);
}

static inline uint32_t rb_space(void)
{
    return UART_RB_SIZE - rb_count() - 1;
}

size_t uart_rb_write(const uint8_t *data, size_t len)
{
    if (data == NULL || len == 0) {
        return 0;
    }

    size_t written = 0;
    uint32_t space;
    uint32_t dropped = 0;

    taskENTER_CRITICAL();
    space = rb_space();

    if (len > space) {
        dropped = len - space;
        len = space;
        rb_stats.buffer_full_count++;
        rb_stats.total_dropped += dropped;
    }

    while (len--) {
        uart_rb[rb_head] = *data++;
        rb_head = (rb_head + 1) % UART_RB_SIZE;
        written++;
    }

    rb_stats.total_written += written;
    rb_stats.current_used = rb_count();

    taskEXIT_CRITICAL();

    uart_rb_start_dma();

    return written;
}

/* LwIP 平台诊断宏（LWIP_PLATFORM_DIAG）的重定向目标：
 * LwIP 传入的是“完整带括号的 printf 参数列表”，无法在宏内用 vsnprintf 直接
 * 消费，故统一收敛为可变参数函数：先格式化到栈上临时缓冲，再经环形缓冲 +
 * DMA 输出（ISR/任意线程上下文均安全，不会因 printf 未被重定向而丢失）。 */
void vUartRbPrintf(const char *fmt, ...)
{
    char buf[160];
    int n;
    va_list ap;

    if (fmt == NULL) {
        return;
    }

    va_start(ap, fmt);
    n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    /* vsnprintf 返回“所需长度”（可能超过缓冲）；缓冲内实际有效字符
     * 最多 sizeof(buf)-1（尾部为 NUL），此处截断避免把 NUL 字符发出去。 */
    if (n > (int)sizeof(buf) - 1) {
        n = (int)sizeof(buf) - 1;
    }

    if (n > 0) {
        /* 若 CLI 当前处于“用户输入模式”（正显示提示符 "> " 与已输入内容），
         * 后台任务（echo server / lwip_test / LwIP 诊断等）经本函数输出会糊在
         * 提示符那一行。此时：
         *   1) 先用 \x1B[2K\r 清掉整行提示符（抹掉 "> " 及已输入内容）；
         *   2) 打印消息；
         *   3) 消息末尾补一个 \r\n，让提示符在下一行重新出现。
         * 关键：上述三段必须“拼成一段、一次性 uart_rb_write”，不能用三次分开
         * 的写——否则 CLI 重绘提示符的 ">" 会插入到“消息”和“换行”之间，
         * 造成 ".....> [ECHO_RX]..." 这类糊行。一次性写入后，CLI 的 ">" 只会
         * 落在整段之前（被 escape 清掉）或之后（自然在下一行），不再插进中间。
         * 命令执行期间 xInUserInputMode 已被置为 pdFALSE，故不会误触发。 */
        if (serial_get_input_mode() == pdTRUE) {
            char out[200];
            int oi = 0;

            /* 1) 清整行 + 回行首 */
            memcpy(out, "\x1B[2K\r", 5);
            oi = 5;

            /* 2) 消息本体 */
            memcpy(out + oi, buf, (size_t)n);
            oi += n;

            /* 3) 确保消息独占一行：末尾必须是 \r\n。
             *    - 已以 \r\n 结尾：保持；
             *    - 以孤立 \n 结尾：改成 \r\n；
             *    - 其它结尾：补 \r\n。 */
            if (buf[n - 1] == '\n') {
                if (n < 2 || buf[n - 2] != '\r') {
                    out[oi - 1] = '\r';
                    out[oi++] = '\n';
                }
            } else {
                out[oi++] = '\r';
                out[oi++] = '\n';
            }

            uart_rb_write((const uint8_t *)out, (size_t)oi);  /* 一次性原子写入 */
            vSetPromptRefreshNeeded();   /* CLI 循环重绘 "> " + 已输入内容 */
        } else {
            uart_rb_write((const uint8_t *)buf, (size_t)n);
        }
    }
}

static volatile uint32_t rb_dma_start_tail = 0;
static volatile uint32_t rb_dma_transfer_count = 0;

void uart_rb_start_dma(void)
{
    UART_HandleTypeDef *huart = s_context.huart;
    uint32_t h, t, count;

    if (huart == NULL || huart->Instance == NULL) {
        return;
    }

    if (huart->gState != HAL_UART_STATE_READY) {
        return;
    }

    taskENTER_CRITICAL();

    h = rb_head;
    t = rb_tail;

    if (h == t) {
        rb_stats.current_used = 0;
        taskEXIT_CRITICAL();
        return;
    }

    if (h > t) {
        count = h - t;
    } else {
        count = UART_RB_SIZE - t;
    }

    if (count > 65535) {
        count = 65535;
    }

    rb_dma_start_tail = t;
    rb_dma_transfer_count = count;
    rb_stats.dma_tx_count++;

    taskEXIT_CRITICAL();

    HAL_UART_Transmit_DMA(huart, (uint8_t *)&uart_rb[t], count);
}

void uart_rb_start_dma_from_isr(void)
{
    UART_HandleTypeDef *huart = s_context.huart;
    uint32_t h, t, count;
    UBaseType_t uxSaved;

    if (huart == NULL || huart->Instance == NULL) {
        return;
    }

    if (huart->gState != HAL_UART_STATE_READY) {
        return;
    }

    uxSaved = taskENTER_CRITICAL_FROM_ISR();

    h = rb_head;
    t = rb_tail;

    if (h == t) {
        rb_stats.current_used = 0;
        taskEXIT_CRITICAL_FROM_ISR(uxSaved);
        return;
    }

    if (h > t) {
        count = h - t;
    } else {
        count = UART_RB_SIZE - t;
    }

    if (count > 65535) {
        count = 65535;
    }

    rb_dma_start_tail = t;
    rb_dma_transfer_count = count;
    rb_stats.dma_tx_count++;

    taskEXIT_CRITICAL_FROM_ISR(uxSaved);

    HAL_UART_Transmit_DMA(huart, (uint8_t *)&uart_rb[t], count);
}

void uart_rb_on_dma_complete(void)
{
    UBaseType_t uxSavedInterruptStatus = portSET_INTERRUPT_MASK_FROM_ISR();
    
    rb_tail = (rb_dma_start_tail + rb_dma_transfer_count) % UART_RB_SIZE;
    rb_stats.current_used = rb_count();
    
    portCLEAR_INTERRUPT_MASK_FROM_ISR(uxSavedInterruptStatus);
    
    uart_rb_start_dma_from_isr();
}

uint32_t uart_rb_available(void)
{
    return rb_space();
}

uint32_t uart_rb_used(void)
{
    return rb_count();
}

void uart_rb_get_stats(uart_rb_stats_t *stats)
{
    if (stats == NULL) return;

    taskENTER_CRITICAL();
    stats->total_written = rb_stats.total_written;
    stats->total_dropped = rb_stats.total_dropped;
    stats->dma_tx_count = rb_stats.dma_tx_count;
    stats->buffer_full_count = rb_stats.buffer_full_count;
    stats->current_used = rb_count();
    taskEXIT_CRITICAL();
}

void uart_rb_reset_stats(void)
{
    taskENTER_CRITICAL();
    rb_stats.total_written = 0;
    rb_stats.total_dropped = 0;
    rb_stats.dma_tx_count = 0;
    rb_stats.buffer_full_count = 0;
    rb_stats.current_used = 0;
    taskEXIT_CRITICAL();
}
