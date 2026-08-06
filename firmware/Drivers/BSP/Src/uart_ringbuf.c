#include "uart_ringbuf.h"
#include "stm32f4xx_hal.h"
#include "FreeRTOS.h"
#include "task.h"
#include <string.h>

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
