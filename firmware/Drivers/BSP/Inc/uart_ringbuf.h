#ifndef _UART_RINGBUF_H_
#define _UART_RINGBUF_H_

#include <stdint.h>
#include <stddef.h>
#include "stm32f4xx_hal.h"

#define UART_RB_SIZE (16384>>1)

typedef struct {
    uint32_t total_written;
    uint32_t total_dropped;
    uint32_t dma_tx_count;
    uint32_t buffer_full_count;
    uint32_t current_used;
} uart_rb_stats_t;

typedef struct {
    UART_HandleTypeDef *huart;
    uint8_t *buffer;
    size_t size;
} uart_rb_context_t;

int uart_rb_register(UART_HandleTypeDef *huart);
const uart_rb_context_t *uart_rb_get_context(void);
void uart_rb_init(void);
size_t uart_rb_write(const uint8_t *data, size_t len);
void uart_rb_start_dma(void);
void uart_rb_start_dma_from_isr(void);
uint32_t uart_rb_available(void);
uint32_t uart_rb_used(void);
void uart_rb_get_stats(uart_rb_stats_t *stats);
void uart_rb_reset_stats(void);
void uart_rb_on_dma_complete(void);

#endif
