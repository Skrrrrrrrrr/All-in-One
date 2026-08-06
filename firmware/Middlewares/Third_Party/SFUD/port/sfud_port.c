/*
 * This file is part of the Serial Flash Universal Driver Library.
 *
 * Copyright (c) 2016-2018, Armink, <armink.ztl@gmail.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * 'Software'), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED 'AS IS', WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 * Function: Portable interface for each platform.
 * Created on: 2016-04-23
 */

#include <sfud.h>
#include <stdarg.h>
#include <string.h>
#include "stm32f4xx_hal.h"
#include "cmsis_os.h"
#include "main.h"

typedef void (*sfud_log_cb_t)(const char *msg);

typedef struct {
    SPI_HandleTypeDef *spi;
    osMutexId_t mutex;
    sfud_log_cb_t log_cb;
} sfud_port_context_t;

static sfud_port_context_t s_context = {
    .spi = NULL,
    .mutex = NULL,
    .log_cb = NULL,
};

static char log_buf[256];

void sfud_log_debug(const char *file, const long line, const char *format, ...);
void sfud_log_info(const char *format, ...);

#define SFUD_SPI_DUMMY_BUF_SIZE  512
#define SFUD_SPI_DUMMY_BYTE      0xFF
#define SPI_TIMEOUT 1000

static uint8_t spi_dummy_buf[SFUD_SPI_DUMMY_BUF_SIZE];
#ifdef SFUD_USING_SPI_DMA
static uint8_t spi_dma_rx_combined[SFUD_SPI_DUMMY_BUF_SIZE];
#endif
static uint8_t spi_buf_initialized = 0;

static void spi_buf_init(void)
{
    if (!spi_buf_initialized) {
        memset(spi_dummy_buf, SFUD_SPI_DUMMY_BYTE, sizeof(spi_dummy_buf));
        spi_buf_initialized = 1;
    }
}

#ifdef osDelay
#define sfud_delay_ms(ms)  osDelay(ms)
#else
#define sfud_delay_ms(ms)  do { uint32_t i; for (i = 0; i < (ms) * 1000; i++) { __NOP(); } } while(0)
#endif

int sfud_port_register(SPI_HandleTypeDef *spi, osMutexId_t mutex, sfud_log_cb_t log_cb)
{
    if (spi == NULL || spi->Instance == NULL || mutex == NULL) {
        return -1;
    }
    s_context.spi = spi;
    s_context.mutex = mutex;
    s_context.log_cb = log_cb;
    return 0;
}

const sfud_port_context_t *sfud_port_get_context(void)
{
    return &s_context;
}

sfud_err sfud_spi_flash_reset(sfud_flash *flash)
{
    sfud_err result = SFUD_SUCCESS;
    uint8_t cmd;
    uint8_t rx_buf[4];

    cmd = SFUD_CMD_ENABLE_RESET;
    result = flash->spi.wr(flash->user_data, &cmd, 1, NULL, 0);
    if (result != SFUD_SUCCESS) {
        return result;
    }

    cmd = SFUD_CMD_RESET;
    result = flash->spi.wr(flash->user_data, &cmd, 1, NULL, 0);
    if (result != SFUD_SUCCESS) {
        return result;
    }

    sfud_delay_ms(1);

    cmd = SFUD_CMD_JEDEC_ID;
    result = flash->spi.wr(flash->user_data, &cmd, 1, rx_buf, 3);
    if (result != SFUD_SUCCESS) {
        return result;
    }

    if (rx_buf[0] == 0xFF && rx_buf[1] == 0xFF && rx_buf[2] == 0xFF) {
        return SFUD_ERR_READ;
    }

    return SFUD_SUCCESS;
}

#ifdef SFUD_USING_SPI_DMA
static osSemaphoreId spiDmaSemaphore;
static volatile sfud_err spiDmaResult = SFUD_SUCCESS;

static void spi_dma_sem_init(void)
{
    if (spiDmaSemaphore == NULL) {
        const osSemaphoreAttr_t semAttr = {
            .name = "spi_dma_sem",
            .attr_bits = 0,
            .cb_mem = NULL,
            .cb_size = 0U,
        };

        spiDmaSemaphore = osSemaphoreNew(1, 0, &semAttr);
        memset(spi_dummy_buf, SFUD_SPI_DUMMY_BYTE, sizeof(spi_dummy_buf));
    }
}

void sfud_spi_dma_txrx_complete(SPI_HandleTypeDef *hspi)
{
    SPI_HandleTypeDef *spi = s_context.spi;
    if (spi != NULL && hspi->Instance == spi->Instance && spiDmaSemaphore != NULL) {
        osSemaphoreRelease(spiDmaSemaphore);
    }
}

void sfud_spi_dma_error(SPI_HandleTypeDef *hspi)
{
    SPI_HandleTypeDef *spi = s_context.spi;
    if (spi != NULL && hspi->Instance == spi->Instance) {
        spiDmaResult = SFUD_ERR_TIMEOUT;
        SFUD_INFO("SPI DMA error! State=0x%08X, ErrorCode=0x%08X",
                  hspi->State, hspi->ErrorCode);
        if (spiDmaSemaphore != NULL) {
            osSemaphoreRelease(spiDmaSemaphore);
        }
    }
}

static sfud_err spi_dma_transmit_receive(const uint8_t *tx_buf, uint8_t *rx_buf, size_t size)
{
    SPI_HandleTypeDef *spi = s_context.spi;
    const uint8_t *actual_tx;
    uint8_t *actual_rx;

    actual_tx = tx_buf ? tx_buf : spi_dummy_buf;
    actual_rx = rx_buf ? rx_buf : spi_dummy_buf;

    spiDmaResult = SFUD_SUCCESS;

    if (HAL_SPI_TransmitReceive_DMA(spi, (uint8_t *)actual_tx, actual_rx, size) != HAL_OK) {
        SFUD_INFO("SPI DMA TxRx start failed!");
        return SFUD_ERR_TIMEOUT;
    }

    if (osSemaphoreAcquire(spiDmaSemaphore, pdMS_TO_TICKS(SPI_TIMEOUT)) != osOK) {
        HAL_SPI_Abort(spi);
        SFUD_INFO("SPI DMA TxRx timeout!");
        return SFUD_ERR_TIMEOUT;
    }

    return spiDmaResult;
}
#endif

static void spi_lock(const sfud_spi *spi)
{
    (void)spi;
    if (s_context.mutex != NULL) {
        osMutexAcquire(s_context.mutex, pdMS_TO_TICKS(100));
    }
}

static void spi_unlock(const sfud_spi *spi)
{
    (void)spi;
    if (s_context.mutex != NULL) {
        osMutexRelease(s_context.mutex);
    }
}

static sfud_err spi_write_read(const sfud_spi *spi, const uint8_t *write_buf, size_t write_size, uint8_t *read_buf,
        size_t read_size) {
    sfud_err result = SFUD_SUCCESS;
    SPI_HandleTypeDef *hspi = s_context.spi;

    HAL_GPIO_WritePin(FLASH_CS_GPIO_Port, FLASH_CS_Pin, GPIO_PIN_RESET);

#ifdef SFUD_USING_SPI_DMA
    if (write_size > 0 && read_size == 0) {
        result = spi_dma_transmit_receive(write_buf, NULL, write_size);
    } else if (write_size > 0 && read_size > 0) {
        size_t total_size = write_size + read_size;
        if (total_size <= SFUD_SPI_DUMMY_BUF_SIZE) {
            memcpy(spi_dummy_buf, write_buf, write_size);
            memset(spi_dummy_buf + write_size, SFUD_SPI_DUMMY_BYTE, read_size);

            result = spi_dma_transmit_receive(spi_dummy_buf, spi_dma_rx_combined, total_size);
            if (result == SFUD_SUCCESS) {
                memcpy(read_buf, spi_dma_rx_combined + write_size, read_size);
            }
        } else {
            SFUD_INFO("SPI DMA size %u exceeds buf %u!",
                      (unsigned)total_size, (unsigned)SFUD_SPI_DUMMY_BUF_SIZE);
            result = SFUD_ERR_TIMEOUT;
        }
    } else if (read_size > 0) {
        result = spi_dma_transmit_receive(NULL, read_buf, read_size);
    }
#else

    if (write_size > 0 && read_size == 0) {
        uint8_t dummy_rx[32];
        if (HAL_SPI_TransmitReceive(hspi, (uint8_t*) write_buf,
                dummy_rx, write_size, SPI_TIMEOUT) != HAL_OK) {
            result = SFUD_ERR_TIMEOUT;
        }
    } else if (write_size > 0 && read_size > 0) {
        size_t total_size = write_size + read_size;
        if (total_size <= SFUD_SPI_DUMMY_BUF_SIZE) {
            memcpy(spi_dummy_buf, write_buf, write_size);
            memset(spi_dummy_buf + write_size, SFUD_SPI_DUMMY_BYTE, read_size);

            uint8_t rx_combined[SFUD_SPI_DUMMY_BUF_SIZE];
            if (HAL_SPI_TransmitReceive(hspi, spi_dummy_buf,
                    rx_combined, total_size, SPI_TIMEOUT) != HAL_OK) {
                result = SFUD_ERR_TIMEOUT;
            } else {
                memcpy(read_buf, rx_combined + write_size, read_size);
            }
        } else {
            SFUD_INFO("SPI size %u exceeds buf %u!", (unsigned )total_size,
                    (unsigned)SFUD_SPI_DUMMY_BUF_SIZE);
            result = SFUD_ERR_TIMEOUT;
        }
    } else if (read_size > 0) {
        memset(spi_dummy_buf, SFUD_SPI_DUMMY_BYTE, read_size);
        if (HAL_SPI_TransmitReceive(hspi, spi_dummy_buf, read_buf,
                read_size, SPI_TIMEOUT) != HAL_OK) {
            result = SFUD_ERR_TIMEOUT;
        }
    }

#endif

    HAL_GPIO_WritePin(FLASH_CS_GPIO_Port, FLASH_CS_Pin, GPIO_PIN_SET);
    return result;
}

#ifdef SFUD_USING_QSPI
static sfud_err qspi_read(const struct __sfud_spi *spi, uint32_t addr, sfud_qspi_read_cmd_format *qspi_read_cmd_format,
        uint8_t *read_buf, size_t read_size) {
    sfud_err result = SFUD_SUCCESS;
    return result;
}
#endif

static void retry_delay(void) {
    osDelay(1);
}

sfud_err sfud_spi_port_init(sfud_flash *flash) {
    sfud_err result = SFUD_SUCCESS;

    if (s_context.spi == NULL || s_context.mutex == NULL) {
        SFUD_INFO("SPI port not registered!");
        return SFUD_ERR_TIMEOUT;
    }

    switch (flash->index) {
    case SFUD_W25Q32BV_DEVICE_INDEX:
    case SFUD_W25Q128BV_DEVICE_INDEX: {
        flash->spi.wr = spi_write_read;
#ifdef SFUD_USING_QSPI
        flash->spi.qspi_read = qspi_read;
#endif
        flash->spi.lock = spi_lock;
        flash->spi.unlock = spi_unlock;
        flash->spi.user_data = s_context.spi;
        flash->retry.delay = retry_delay;
        flash->retry.times = 10 * 1000;
        break;
    }
    default:
        SFUD_INFO("Unknown flash device index: %d", flash->index);
        return SFUD_ERR_TIMEOUT;
    }
#ifdef SFUD_USING_SPI_DMA
    spi_dma_sem_init();
#else
    spi_buf_init();
#endif

    result = sfud_spi_flash_reset(flash);
    if (result != SFUD_SUCCESS) {
        SFUD_INFO("SPI flash reset failed: %d", result);
        return result;
    }
    SFUD_INFO("SPI flash reset OK");

    return result;
}

void sfud_log_debug(const char *file, const long line, const char *format, ...) {
    va_list args;
    int offset;

    va_start(args, format);
    offset = snprintf(log_buf, sizeof(log_buf), "[SFUD](%s:%ld) ", file, line);
    vsnprintf(log_buf + offset, sizeof(log_buf) - offset, format, args);
    strcat(log_buf, "\r\n");
    if (s_context.log_cb != NULL) {
        s_context.log_cb(log_buf);
    }
    va_end(args);
}

void sfud_log_info(const char *format, ...) {
    va_list args;
    int offset;

    va_start(args, format);
    offset = snprintf(log_buf, sizeof(log_buf), "[SFUD]");
    vsnprintf(log_buf + offset, sizeof(log_buf) - offset, format, args);
    strcat(log_buf, "\r\n");
    if (s_context.log_cb != NULL) {
        s_context.log_cb(log_buf);
    }
    va_end(args);
}
