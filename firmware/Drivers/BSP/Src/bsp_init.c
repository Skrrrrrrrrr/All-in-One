/*
 * bsp_init.c
 * BSP初始化中心模块，负责各驱动模块的注册和初始化
 * 初始化顺序：uart_ringbuf → sfud → lfs → elog → CLI
 */

#include "bsp_init.h"
#include "uart_ringbuf.h"
#include "sfud.h"
#include "lfs_port.h"
#include "elog.h"
#include "serial.h"
#include "cmsis_os.h"
#include "stm32f4xx_hal.h"
#include "lfs.h"

typedef void (*sfud_log_cb_t)(const char *msg);
int sfud_port_register(SPI_HandleTypeDef *spi, osMutexId_t mutex, sfud_log_cb_t log_cb);

typedef void (*elog_output_cb_t)(const uint8_t *data, size_t size);
typedef void (*elog_prompt_refresh_cb_t)(void);
typedef BaseType_t (*elog_input_mode_check_cb_t)(void);
typedef uint8_t (*elog_input_len_get_cb_t)(void);
typedef void (*elog_input_get_cb_t)(char *buf, uint8_t len);

int elog_port_register(elog_output_cb_t output_cb,
                       elog_prompt_refresh_cb_t prompt_refresh_cb,
                       elog_input_mode_check_cb_t input_mode_check_cb,
                       elog_input_len_get_cb_t input_len_get_cb,
                       elog_input_get_cb_t input_get_cb);

static osMutexId_t spiMutex = NULL;

static void elog_output_cb(const uint8_t *data, size_t size)
{
    uart_rb_write((uint8_t *)data, size);
}

static void elog_prompt_refresh_cb(void)
{
    vSetPromptRefreshNeeded();
}

static BaseType_t elog_input_mode_check_cb(void)
{
    return serial_get_input_mode();
}

static uint8_t elog_input_len_get_cb(void)
{
    return serial_get_input_len();
}

static void elog_input_get_cb(char *buf, uint8_t len)
{
    serial_get_input_string(buf, len);
}

static void sfud_log_cb(const char *msg)
{
    elog_output_cb((const uint8_t *)msg, strlen(msg));
}

int bsp_init(UART_HandleTypeDef *huart, SPI_HandleTypeDef *hspi)
{
    int ret;
    const osMutexAttr_t mutexAttr = {
        .name = "spi_mutex",
        .attr_bits = osMutexRecursive,
        .cb_mem = NULL,
        .cb_size = 0U,
    };

    ret = uart_rb_register(huart);
    if (ret != 0) {
        return -2;
    }
    uart_rb_init();
    uart_rb_write((uint8_t*) "[BSP] OK: uart_rb initialized\r\n", sizeof("[BSP] OK: uart_rb initialized\r\n"));

    spiMutex = osMutexNew(&mutexAttr);
    if (spiMutex == NULL) {
        uart_rb_write((uint8_t*) "[BSP] Failed: spiMutex creation\r\n",  sizeof("[BSP] Failed: spiMutex creation\r\n"));
        return -1;
    }
    uart_rb_write((uint8_t*) "[BSP] OK: spiMutex created\r\n", sizeof("[BSP] OK: spiMutex created\r\n"));

    ret = sfud_port_register(hspi, spiMutex, sfud_log_cb);
    if (ret != 0) {
        uart_rb_write((uint8_t*) "[BSP] Failed: sfud_port_register\r\n", sizeof("[BSP] Failed: sfud_port_register\r\n"));
        return -3;
    }
    uart_rb_write((uint8_t*) "[BSP] OK: sfud_port_register\r\n", sizeof("[BSP] OK: sfud_port_register\r\n"));

    ret = sfud_init();
    if (ret != SFUD_SUCCESS) {
        uart_rb_write((uint8_t*) "[BSP] Failed: sfud_init\r\n", sizeof("[BSP] Failed: sfud_init\r\n"));
        return -4;
    }
    uart_rb_write((uint8_t*) "[BSP] OK: sfud_init\r\n", sizeof("[BSP] OK: sfud_init\r\n"));

    ret = lfs_port_register(sfud_get_device(SFUD_W25Q128BV_DEVICE_INDEX));
    if (ret != 0) {
        uart_rb_write((uint8_t*) "[BSP] Failed: lfs_port_register - flash is NULL\r\n", sizeof("[BSP] Failed: lfs_port_register - flash is NULL\r\n"));
        return -5;
    }
    uart_rb_write((uint8_t*) "[BSP] OK: lfs_port_register\r\n", sizeof("[BSP] OK: lfs_port_register\r\n"));

    ret = lfs_spi_flash_init(lfs_port_get_lfs_config());
    if (ret != LFS_ERR_OK) {
        uart_rb_write((uint8_t*) "[BSP] Failed: lfs_spi_flash_init\r\n", sizeof("[BSP] Failed: lfs_spi_flash_init\r\n"));
        return -6;
    }
    uart_rb_write((uint8_t*) "[BSP] OK: lfs_spi_flash_init\r\n", sizeof("[BSP] OK: lfs_spi_flash_init\r\n"));

    ret = lfs_mount(lfs_port_get_lfs(), lfs_port_get_lfs_config());
    if (ret != LFS_ERR_OK) {
        uart_rb_write((uint8_t*) "[BSP] lfs_mount failed, trying format...\r\n", sizeof("[BSP] lfs_mount failed, trying format...\r\n"));
        
        sfud_flash *flash = lfs_port_get_flash();
        if (flash != NULL) {
            uint32_t block_size = flash->chip.erase_gran;
            sfud_erase(flash, 0x0, block_size);
            uart_rb_write((uint8_t*) "[BSP] OK: erased first block for reformat\r\n", sizeof("[BSP] OK: erased first block for reformat\r\n"));
        }
        
        ret = lfs_format(lfs_port_get_lfs(), lfs_port_get_lfs_config());
        if (ret != LFS_ERR_OK) {
            uart_rb_write((uint8_t*) "[BSP] Failed: lfs_format\r\n", sizeof("[BSP] Failed: lfs_format\r\n"));
            return -7;
        }
        uart_rb_write((uint8_t*) "[BSP] OK: lfs_format\r\n", sizeof("[BSP] OK: lfs_format\r\n"));
        ret = lfs_mount(lfs_port_get_lfs(), lfs_port_get_lfs_config());
        if (ret != LFS_ERR_OK) {
            uart_rb_write((uint8_t*) "[BSP] Failed: lfs_mount after format\r\n", sizeof("[BSP] Failed: lfs_mount after format\r\n"));
            return -8;
        }
    }
    uart_rb_write((uint8_t*) "[BSP] OK: lfs_mount\r\n", sizeof("[BSP] OK: lfs_mount\r\n"));

    ret = elog_port_register(elog_output_cb,
                             elog_prompt_refresh_cb,
                             elog_input_mode_check_cb,
                             elog_input_len_get_cb,
                             elog_input_get_cb);
    if (ret != 0) {
        uart_rb_write((uint8_t*) "[BSP] Failed: elog_port_register\r\n", sizeof("[BSP] Failed: elog_port_register\r\n"));
        return -6;
    }
    uart_rb_write((uint8_t*) "[BSP] OK: elog_port_register\r\n", sizeof("[BSP] OK: elog_port_register\r\n"));

    elog_init();
    uart_rb_write((uint8_t*) "[BSP] OK: elog_init\r\n", sizeof("[BSP] OK: elog_init\r\n"));
    
    elog_set_text_color_enabled(true);
    uart_rb_write((uint8_t*) "[BSP] OK: elog_set_text_color_enabled\r\n", sizeof("[BSP] OK: elog_set_text_color_enabled\r\n"));
    
    elog_start();
    uart_rb_write((uint8_t*) "[BSP] OK: elog_start\r\n", sizeof("[BSP] OK: elog_start\r\n"));

    uart_rb_write((uint8_t*) "[BSP] bsp_init completed successfully!\r\n", sizeof("[BSP] bsp_init completed successfully!\r\n"));

    return 0;
}
