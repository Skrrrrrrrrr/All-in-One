/*
 * ota_core.h
 *
 * OTA核心逻辑：参数区管理（双槽冗余）、下载状态机、校验、激活、回滚。
 *
 * 升级流程（A/B双区）:
 *   begin(擦除非活跃区) → write(分片写) → end(CRC32校验) → activate(置启动标志)
 *   → 重启 → Bootloader引导新分区 → App运行成功调用 ota_confirm_ok()
 *   → 新固件异常(看门狗复位)且 boot_attempts 超限 → Bootloader自动回滚旧区
 *
 * 断电保护：
 *   - 每次Flash写前检查PVDO，电压过低立即中止
 *   - 参数区双槽冗余，断电不会导致启动标志永久损坏
 *
 * 日志：所有关键步骤（开始/写块/校验/完成/失败）通过EasyLogger记录。
 */

#ifndef __OTA_CORE_H__
#define __OTA_CORE_H__

#include <stdint.h>
#include <stddef.h>
#include "ota_flash_port.h"

#ifdef __cplusplus
extern "C" {
#endif

/* --------------------------- 参数区结构 --------------------------- */
#define OTA_PARAMS_MAGIC        (0x4F54414Du)   /* "OTAM" */
#define OTA_PARAMS_VERSION      (1u)
#define OTA_SLOT_INVALID        (0xFFu)         /* pending_slot无待验证区 */
#define OTA_MAX_BOOT_ATTEMPTS   (3u)            /* 新固件最大启动尝试次数 */

typedef struct {
    uint32_t magic;         /* OTA_PARAMS_MAGIC */
    uint32_t version;       /* OTA_PARAMS_VERSION */
    uint32_t crc32;         /* 校验从magic到reserved2（计算时crc32置0） */
    uint8_t  active_slot;   /* 当前引导区（0=A, 1=B） */
    uint8_t  pending_slot;  /* 待验证区（OTA_SLOT_INVALID=无） */
    uint8_t  boot_attempts; /* 新固件已尝试启动次数 */
    uint8_t  reserved1;
    uint32_t fw_size;       /* 待验证固件大小 */
    uint32_t fw_crc32;      /* 待验证固件CRC32 */
    uint32_t fw_version;    /* 待验证固件版本 */
    uint32_t reserved2;
} ota_params_t;             /* 40字节，4字节对齐 */

/* --------------------------- 状态机 --------------------------- */
typedef enum {
    OTA_STATE_IDLE = 0,     /* 空闲，可开始升级 */
    OTA_STATE_WRITING,      /* 固件写入中 */
    OTA_STATE_VERIFYING,    /* 校验中（短暂） */
    OTA_STATE_READY,        /* 校验通过，待激活 */
    OTA_STATE_ABORTED,      /* 已中止 */
    OTA_STATE_ERROR,        /* 错误 */
} ota_state_t;

/* ---------------------------- 初始化 ---------------------------- */
void ota_init(void);

/* 启动vOTATask（静态分配，可在调度器启动后调用一次）。返回OTA_ERR_OK。 */
int ota_task_start(void);

/* 由FreeRTOS调度器调用 */
//void vOTATask(void *argument);

/* ------------------------ 核心流程（供任务调用） ------------------------ */
int ota_begin(uint32_t fw_size, uint32_t fw_crc32, uint32_t fw_version);
int ota_write_chunk(uint32_t offset, const uint8_t *data, size_t len);
int ota_end(void);
int ota_activate(void);
int ota_rollback(void);

/* App启动成功后调用：确认当前固件运行正常，清除待验证状态 */
void ota_confirm_ok(void);

/* -------------------------- 状态查询 -------------------------- */
ota_state_t ota_get_state(void);
ota_slot_t ota_get_active_slot(void);
ota_slot_t ota_get_target_slot(void);
uint32_t ota_get_fw_size(void);
uint32_t ota_get_fw_crc32(void);
uint32_t ota_get_written(void);
void ota_get_status_string(char *buf, size_t len);

/* --------------------- CLI同步请求（阻塞等待结果） --------------------- */
int ota_cli_begin(uint32_t size, uint32_t crc, uint32_t version);
int ota_cli_write(uint32_t offset, const uint8_t *data, uint16_t len);
int ota_cli_end(void);
int ota_cli_activate(void);
int ota_cli_rollback(void);
int ota_cli_confirm(void);

#ifdef __cplusplus
}
#endif

#endif /* __OTA_CORE_H__ */
