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

static char log_buf[256];

void sfud_log_debug(const char *file, const long line, const char *format, ...);

#include"main.h"
extern SPI_HandleTypeDef hspi3;
#include "cmsis_os.h"
// 引用 CubeMX 生成的互斥量句柄
extern osMutexId spiMutexHandle;
#define osMutexTimeOut 100

#define SFUD_SPI_HANDLER hspi3
#define SPI_TIMEOUT 1000


#define retry_delay HAL
static void spi_lock(const sfud_spi *spi)
{
	//retry_delay使用RTOS延迟，系统中断不能关闭！
    //__disable_irq();
    osMutexAcquire(spiMutexHandle, pdMS_TO_TICKS(100));
}

static void spi_unlock(const sfud_spi *spi)
{
	//retry_delay使用了RTOS延迟，系统中断不能关闭！
	//__enable_irq();
    osMutexRelease(spiMutexHandle);
}


/**
 * SPI write data then read data
 */
static sfud_err spi_write_read(const sfud_spi *spi, const uint8_t *write_buf, size_t write_size, uint8_t *read_buf,
        size_t read_size) {
    sfud_err result = SFUD_SUCCESS;
//    uint8_t send_data, read_data;

    /**
     * add your spi write and read code
     */
    HAL_GPIO_WritePin(FLASH_CS_GPIO_Port, FLASH_CS_Pin, GPIO_PIN_RESET);

	if (write_size && read_size == 0)
	{
		// 仅写
		if (HAL_SPI_Transmit(&SFUD_SPI_HANDLER, (uint8_t *)write_buf, write_size, SPI_TIMEOUT) != HAL_OK)
		{
			result = SFUD_ERR_TIMEOUT;
		}
	}
	else if (write_size && read_size)
	{
		// 先写再读（常见的读命令流程）
		if (HAL_SPI_Transmit(&SFUD_SPI_HANDLER, (uint8_t *)write_buf, write_size, SPI_TIMEOUT) != HAL_OK ||
			HAL_SPI_Receive(&SFUD_SPI_HANDLER, read_buf, read_size, SPI_TIMEOUT) != HAL_OK)
		{
			result = SFUD_ERR_TIMEOUT;
		}
	}
	else if (write_size == 0 && read_size)
	{
		// 仅读（几乎不会用到）
		if (HAL_SPI_Receive(&SFUD_SPI_HANDLER, read_buf, read_size, SPI_TIMEOUT) != HAL_OK)
		{
			result = SFUD_ERR_TIMEOUT;
		}
	}

	HAL_GPIO_WritePin(FLASH_CS_GPIO_Port, FLASH_CS_Pin, GPIO_PIN_SET);


    return result;
}

#ifdef SFUD_USING_QSPI
/**
 * read flash data by QSPI
 */
static sfud_err qspi_read(const struct __sfud_spi *spi, uint32_t addr, sfud_qspi_read_cmd_format *qspi_read_cmd_format,
        uint8_t *read_buf, size_t read_size) {
    sfud_err result = SFUD_SUCCESS;

    /**
     * add your qspi read flash data code
     */

    return result;
}
#endif /* SFUD_USING_QSPI */

#include "cmsis_os.h"
static void retry_delay(void) {
    // 让当前任务休眠 1 个系统 Tick（通常为 1ms 或 10ms）
    osDelay(1);
}

sfud_err sfud_spi_port_init(sfud_flash *flash) {
    sfud_err result = SFUD_SUCCESS;

    /**
     * add your port spi bus and device object initialize code like this:
     * 1. rcc initialize
     * 2. gpio initialize
     * 3. spi device initialize
     * 4. flash->spi and flash->retry item initialize
     *    flash->spi.wr = spi_write_read; //Required
     *    flash->spi.qspi_read = qspi_read; //Required when QSPI mode enable
     *    flash->spi.lock = spi_lock;
     *    flash->spi.unlock = spi_unlock;
     *    flash->spi.user_data = &spix;
     *    flash->retry.delay = null;
     *    flash->retry.times = 10000; //Required
     */
	switch (flash->index) {
	case SFUD_W25_DEVICE_INDEX: {
		/* set the interfaces and data */
		flash->spi.wr = spi_write_read;
#ifdef SFUD_USING_QSPI
		flash->spi.qspi_read = qspi_read;
#endif
		flash->spi.lock = spi_lock;
		flash->spi.unlock = spi_unlock;
		flash->spi.user_data = &SFUD_SPI_HANDLER;
		/* about 100 microsecond delay */
		flash->retry.delay = retry_delay;
		/* adout 60 seconds timeout */
		flash->retry.times = 1000;

		break;
	}
	}
    return result;
}

/**
 * This function is print debug info.
 *
 * @param file the file which has call this function
 * @param line the line number which has call this function
 * @param format output format
 * @param ... args
 */
void sfud_log_debug(const char *file, const long line, const char *format, ...) {
    va_list args;

    /* args point to the first variable parameter */
    va_start(args, format);
    printf("[SFUD](%s:%ld) ", file, line);
    /* must use vprintf to print */
    vsnprintf(log_buf, sizeof(log_buf), format, args);
    printf("%s\n", log_buf);
    va_end(args);
}

/**
 * This function is print routine info.
 *
 * @param format output format
 * @param ... args
 */
void sfud_log_info(const char *format, ...) {
    va_list args;

    /* args point to the first variable parameter */
    va_start(args, format);
    printf("[SFUD]");
    /* must use vprintf to print */
    vsnprintf(log_buf, sizeof(log_buf), format, args);
    printf("%s\n", log_buf);
    va_end(args);
}
