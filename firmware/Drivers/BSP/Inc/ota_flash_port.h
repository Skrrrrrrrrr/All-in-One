/*
 * ota_flash_port.h
 *
 * OTA底层Flash驱动：封装内部Flash（HAL FLASH），提供OTA专用的
 * 擦除、分片写入、读取、CRC32校验接口。
 *
 * 分区布局（内部Flash 1MB, 0x08000000~0x08100000, STM32F407ZGTx扇区对齐）:
 *   | 分区       | 起始地址     | 大小   | 扇区       | 用途                        |
 *   |------------|--------------|--------|------------|-----------------------------|
 *   | Bootloader | 0x08000000   | 64KB   | S0-S3      | 引导程序（App侧只读）       |
 *   | App A区    | 0x08010000   | 448KB  | S4-S7      | 应用镜像A（当前运行区）     |
 *   | App B区    | 0x08080000   | 384KB  | S8-S10     | 应用镜像B（升级目标区）     |
 *   | OTA参数区  | 0x080E0000   | 128KB  | S11        | 启动标志+固件信息（双槽冗余）|
 *
 * 固件镜像大小上限 = 384KB（App B区大小）。
 * 注意：部署Bootloader后，App链接脚本 FLASH ORIGIN 必须改为 0x08010000。
 */

#ifndef __OTA_FLASH_PORT_H__
#define __OTA_FLASH_PORT_H__

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------- 内部Flash分区布局 ------------------------- */
#define OTA_BOOTLOADER_ADDR         (0x08000000u)
#define OTA_BOOTLOADER_SIZE         (0x00010000u)   /* 64KB, 扇区S0-S3 */

#define OTA_APP_A_ADDR              (0x08010000u)
#define OTA_APP_A_SIZE              (0x00070000u)   /* 448KB, 扇区S4-S7 */

#define OTA_APP_B_ADDR              (0x08080000u)
#define OTA_APP_B_SIZE              (0x00060000u)   /* 384KB, 扇区S8-S10 */

#define OTA_PARAM_ADDR              (0x080E0000u)
#define OTA_PARAM_SIZE              (0x00020000u)   /* 128KB, 扇区S11 */

#define OTA_PARAM_SLOT0_ADDR        (OTA_PARAM_ADDR)            /* 0x080E0000, 64KB */
#define OTA_PARAM_SLOT1_ADDR        (OTA_PARAM_ADDR + 0x10000u) /* 0x080F0000, 64KB */
#define OTA_PARAM_SLOT_SIZE         (0x00010000u)

/* 固件镜像大小上限（以较小分区为准，保证A/B均可容纳） */
#define OTA_FW_MAX_SIZE             (OTA_APP_B_SIZE)

/* ------------------------------- 槽位 ------------------------------- */
typedef enum {
    OTA_SLOT_A = 0,
    OTA_SLOT_B = 1,
} ota_slot_t;

/* ----------------------------- 错误码 ------------------------------ */
#define OTA_ERR_OK                  (0)
#define OTA_ERR_PARAM               (-1)
#define OTA_ERR_FLASH               (-2)   /* 内部Flash操作失败 */
#define OTA_ERR_POWER_FAIL          (-3)   /* 电源跌落，操作中止 */
#define OTA_ERR_TIMEOUT             (-4)
#define OTA_ERR_STATE               (-5)   /* 状态机不允许当前操作 */
#define OTA_ERR_CRC                 (-6)
#define OTA_ERR_SIZE                (-7)

/* ---------------------------- CRC32校验 ---------------------------- */
/* 完整计算：crc = ota_crc32(data, len) */
uint32_t ota_crc32(const uint8_t *data, size_t len);

/* 增量更新（用于分块校验大固件镜像）：
 *   crc 初值 = 0xFFFFFFFF，每块调用 crc = ota_crc32_update(crc, buf, len)，
 *   全部块结束后 crc ^= 0xFFFFFFFF 即为最终值。 */
uint32_t ota_crc32_update(uint32_t crc, const uint8_t *data, size_t len);

/* --------------------------- 固件区读写 ---------------------------- */
/* 擦除整个应用分区（Sector粒度，带超时；操作前检查电源） */
int ota_flash_erase_app(ota_slot_t slot);

/* 分片写入：offset为分区内偏移，data长度不限（内部4字节对齐处理）。
 * 调用前分区必须已用 ota_flash_erase_app 擦除。写前检查电源。 */
int ota_flash_write_chunk(ota_slot_t slot, uint32_t offset,
                          const uint8_t *data, size_t len);

/* 读出固件（用于校验），返回实际读取字节数；失败返回负错误码 */
int ota_flash_read_app(ota_slot_t slot, uint32_t offset,
                       uint8_t *buf, size_t len);

/* 分区信息 */
uint32_t ota_flash_app_addr(ota_slot_t slot);
uint32_t ota_flash_app_size(ota_slot_t slot);

/* --------------------------- 参数区读写 ---------------------------- */
/* 双槽冗余写：擦除S11整扇区 → 写Slot0 → 写Slot1。任一槽写入完成即有效。
 * 写入前检查电源；断电发生在擦除后 → 双槽均无效（上层回退默认引导A区）。 */
int ota_flash_write_params(const void *params, size_t len);

/* 读指定参数槽（slot: 0或1），原样读出，由上层校验magic/crc选择有效槽 */
int ota_flash_read_param_slot(uint8_t slot, void *params, size_t len);

/* 电源监测：返回1表示电源正常，0表示PVDO置位（电压过低） */
uint8_t ota_power_ok(void);

#ifdef __cplusplus
}
#endif

#endif /* __OTA_FLASH_PORT_H__ */
