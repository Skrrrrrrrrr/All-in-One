/*
 * ota_flash_port.c
 *
 * OTA底层Flash驱动：基于HAL FLASH封装STM32F407内部Flash操作。
 *
 * 设计要点：
 *  1. 擦除/编程使用寄存器级操作（解锁/置位/等待BSY/检查错误），
 *     等待带超时（HAL_GetTick），杜绝无超时的死循环。不依赖HAL
 *     FLASH源文件（该源文件未编译进本工程，链接会失败）。
 *  2. 所有写入前先检查电源（PVDO），电压过低立即中止，防止OTA过程
 *     因断电写坏Flash。
 *  3. 编程单位为WORD(4字节)，分片写入自动做4字节对齐处理。
 *  4. 参数区采用双槽冗余（Slot0/Slot1），任一副本有效即可恢复。
 *
 * 作者: OTA Team
 * 修改记录:
 *   2026-08-07  初始版本
 */

#include "ota_flash_port.h"
#include "stm32f4xx_hal.h"
#include <string.h>

/* ----------------------- 扇区表（F407ZGTx 1MB） -----------------------
 * S0~S3: 16KB x4, S4: 64KB, S5~S11: 128KB x7
 */
typedef struct {
    uint32_t addr;
    uint32_t size;
    uint32_t sector;   /* HAL Sector编号 */
} ota_sector_t;

static const ota_sector_t s_sector_table[] = {
    { 0x08000000u, 0x4000u, FLASH_SECTOR_0  },
    { 0x08004000u, 0x4000u, FLASH_SECTOR_1  },
    { 0x08008000u, 0x4000u, FLASH_SECTOR_2  },
    { 0x0800C000u, 0x4000u, FLASH_SECTOR_3  },
    { 0x08010000u, 0x10000u, FLASH_SECTOR_4 },
    { 0x08020000u, 0x20000u, FLASH_SECTOR_5 },
    { 0x08040000u, 0x20000u, FLASH_SECTOR_6 },
    { 0x08060000u, 0x20000u, FLASH_SECTOR_7 },
    { 0x08080000u, 0x20000u, FLASH_SECTOR_8 },
    { 0x080A0000u, 0x20000u, FLASH_SECTOR_9 },
    { 0x080C0000u, 0x20000u, FLASH_SECTOR_10 },
    { 0x080E0000u, 0x20000u, FLASH_SECTOR_11 },
};

#define OTA_SECTOR_COUNT   (sizeof(s_sector_table) / sizeof(s_sector_table[0]))

/* CRC32表（IEEE 802.3, poly=0xEDB88320），位于Flash const区，不占RAM */
static const uint32_t s_crc32_table[256] = {
    0x00000000u, 0x77073096u, 0xee0e612cu, 0x990951bau, 0x076dc419u, 0x706af48fu,
    0xe963a535u, 0x9e6495a3u, 0x0edb8832u, 0x79dcb8a4u, 0xe0d5e91eu, 0x97d2d988u,
    0x09b64c2bu, 0x7eb17cbdu, 0xe7b82d07u, 0x90bf1d91u, 0x1db71064u, 0x6ab020f2u,
    0xf3b97148u, 0x84be41deu, 0x1adad47du, 0x6ddde4ebu, 0xf4d4b551u, 0x83d385c7u,
    0x136c9856u, 0x646ba8c0u, 0xfd62f97au, 0x8a65c9ecu, 0x14015c4fu, 0x63066cd9u,
    0xfa0f3d63u, 0x8d080df5u, 0x3b6e20c8u, 0x4c69105eu, 0xd56041e4u, 0xa2677172u,
    0x3c03e4d1u, 0x4b04d447u, 0xd20d85fdu, 0xa50ab56bu, 0x35b5a8fau, 0x42b2986cu,
    0xdbbbc9d6u, 0xacbcf940u, 0x32d86ce3u, 0x45df5c75u, 0xdcd60dcfu, 0xabd13d59u,
    0x26d930acu, 0x51de003au, 0xc8d75180u, 0xbfd06116u, 0x21b4f4b5u, 0x56b3c423u,
    0xcfba9599u, 0xb8bda50fu, 0x2802b89eu, 0x5f058808u, 0xc60cd9b2u, 0xb10be924u,
    0x2f6f7c87u, 0x58684c11u, 0xc1611dabu, 0xb6662d3du, 0x76dc4190u, 0x01db7106u,
    0x98d220bcu, 0xefd5102au, 0x71b18589u, 0x06b6b51fu, 0x9fbfe4a5u, 0xe8b8d433u,
    0x7807c9a2u, 0x0f00f934u, 0x9609a88eu, 0xe10e9818u, 0x7f6a0dbbu, 0x086d3d2du,
    0x91646c97u, 0xe6635c01u, 0x6b6b51f4u, 0x1c6c6162u, 0x856530d8u, 0xf262004eu,
    0x6c0695edu, 0x1b01a57bu, 0x8208f4c1u, 0xf50fc457u, 0x65b0d9c6u, 0x12b7e950u,
    0x8bbeb8eau, 0xfcb9887cu, 0x62dd1ddfu, 0x15da2d49u, 0x8cd37cf3u, 0xfbd44c65u,
    0x4db26158u, 0x3ab551ceu, 0xa3bc0074u, 0xd4bb30e2u, 0x4adfa541u, 0x3dd895d7u,
    0xa4d1c46du, 0xd3d6f4fbu, 0x4369e96au, 0x346ed9fcu, 0xad678846u, 0xda60b8d0u,
    0x44042d73u, 0x33031de5u, 0xaa0a4c5fu, 0xdd0d7cc9u, 0x5005713cu, 0x270241aau,
    0xbe0b1010u, 0xc90c2086u, 0x5768b525u, 0x206f85b3u, 0xb966d409u, 0xce61e49fu,
    0x5edef90eu, 0x29d9c998u, 0xb0d09822u, 0xc7d7a8b4u, 0x59b33d17u, 0x2eb40d81u,
    0xb7bd5c3bu, 0xc0ba6cadu, 0xedb88320u, 0x9abfb3b6u, 0x03b6e20cu, 0x74b1d29au,
    0xead54739u, 0x9dd277afu, 0x04db2615u, 0x73dc1683u, 0xe3630b12u, 0x94643b84u,
    0x0d6d6a3eu, 0x7a6a5aa8u, 0xe40ecf0bu, 0x9309ff9du, 0x0a00ae27u, 0x7d079eb1u,
    0xf00f9344u, 0x8708a3d2u, 0x1e01f268u, 0x6906c2feu, 0xf762575du, 0x806567cbu,
    0x196c3671u, 0x6e6b06e7u, 0xfed41b76u, 0x89d32be0u, 0x10da7a5au, 0x67dd4accu,
    0xf9b9df6fu, 0x8ebeeff9u, 0x17b7be43u, 0x60b08ed5u, 0xd6d6a3e8u, 0xa1d1937eu,
    0x38d8c2c4u, 0x4fdff252u, 0xd1bb67f1u, 0xa6bc5767u, 0x3fb506ddu, 0x48b2364bu,
    0xd80d2bdau, 0xaf0a1b4cu, 0x36034af6u, 0x41047a60u, 0xdf60efc3u, 0xa867df55u,
    0x316e8eefu, 0x4669be79u, 0xcb61b38cu, 0xbc66831au, 0x256fd2a0u, 0x5268e236u,
    0xcc0c7795u, 0xbb0b4703u, 0x220216b9u, 0x5505262fu, 0xc5ba3bbeu, 0xb2bd0b28u,
    0x2bb45a92u, 0x5cb36a04u, 0xc2d7ffa7u, 0xb5d0cf31u, 0x2cd99e8bu, 0x5bdeae1du,
    0x9b64c2b0u, 0xec63f226u, 0x756aa39cu, 0x026d930au, 0x9c0906a9u, 0xeb0e363fu,
    0x72076785u, 0x05005713u, 0x95bf4a82u, 0xe2b87a14u, 0x7bb12baeu, 0x0cb61b38u,
    0x92d28e9bu, 0xe5d5be0du, 0x7cdcefb7u, 0x0bdbdf21u, 0x86d3d2d4u, 0xf1d4e242u,
    0x68ddb3f8u, 0x1fda836eu, 0x81be16cdu, 0xf6b9265bu, 0x6fb077e1u, 0x18b74777u,
    0x88085ae6u, 0xff0f6a70u, 0x66063bcau, 0x11010b5cu, 0x8f659effu, 0xf862ae69u,
    0x616bffd3u, 0x166ccf45u, 0xa00ae278u, 0xd70dd2eeu, 0x4e048354u, 0x3903b3c2u,
    0xa7672661u, 0xd06016f7u, 0x4969474du, 0x3e6e77dbu, 0xaed16a4au, 0xd9d65adcu,
    0x40df0b66u, 0x37d83bf0u, 0xa9bcae53u, 0xdebb9ec5u, 0x47b2cf7fu, 0x30b5ffe9u,
    0xbdbdf21cu, 0xcabac28au, 0x53b39330u, 0x24b4a3a6u, 0xbad03605u, 0xcdd70693u,
    0x54de5729u, 0x23d967bfu, 0xb3667a2eu, 0xc4614ab8u, 0x5d681b02u, 0x2a6f2b94u,
    0xb40bbe37u, 0xc30c8ea1u, 0x5a05df1bu, 0x2d02ef8du
};

uint32_t ota_crc32_update(uint32_t crc, const uint8_t *data, size_t len)
{
    size_t i;

    if (data == NULL) {
        return crc;
    }

    for (i = 0; i < len; i++) {
        crc = (crc >> 8) ^ s_crc32_table[(crc ^ data[i]) & 0xFFu];
    }

    return crc;
}

uint32_t ota_crc32(const uint8_t *data, size_t len)
{
    uint32_t crc = 0xFFFFFFFFu;

    if (data == NULL) {
        return 0u;
    }

    crc = ota_crc32_update(crc, data, len);

    return (crc ^ 0xFFFFFFFFu);
}

/* 电源监测：PVDO置位表示VDD低于PVD阈值 */
uint8_t ota_power_ok(void)
{
    return (__HAL_PWR_GET_FLAG(PWR_FLAG_PVDO) == RESET) ? 1u : 0u;
}

uint32_t ota_flash_app_addr(ota_slot_t slot)
{
    return (slot == OTA_SLOT_A) ? OTA_APP_A_ADDR : OTA_APP_B_ADDR;
}

uint32_t ota_flash_app_size(ota_slot_t slot)
{
    return (slot == OTA_SLOT_A) ? OTA_APP_A_SIZE : OTA_APP_B_SIZE;
}

/* ------------------------- 内部Flash原始操作 ------------------------- */

/* 擦除/编程等待超时：F407最大128KB扇区擦除约1~2s，取5s余量 */
#define OTA_FLASH_TIMEOUT_MS   (5000u)

/* F407编程/擦除错误标志组合（旧版CMSIS头文件无FLASH_SR_PGERR，逐位列出） */
#define OTA_FLASH_ERR_MASK     (FLASH_SR_WRPERR | FLASH_SR_PGAERR | \
                                FLASH_SR_PGPERR | FLASH_SR_PGSERR)

/* 等待Flash空闲，基于HAL_GetTick超时；返回0表示超时 */
static uint8_t ota_flash_wait_idle(void)
{
    uint32_t start = HAL_GetTick();

    while ((FLASH->SR & FLASH_SR_BSY) != 0u) {
        if ((HAL_GetTick() - start) >= OTA_FLASH_TIMEOUT_MS) {
            return 0u;
        }
    }
    return 1u;
}

static void ota_flash_unlock(void)
{
    FLASH->KEYR = 0x45670123u;
    FLASH->KEYR = 0xCDEF89ABu;
}

static void ota_flash_lock(void)
{
    FLASH->CR |= FLASH_CR_LOCK;
}

/* 擦除单个扇区（sector: 0~11），带超时与错误检查 */
static int ota_flash_erase_sector(uint32_t sector)
{
    if (sector >= OTA_SECTOR_COUNT) {
        return OTA_ERR_PARAM;
    }
    if (!ota_flash_wait_idle()) {
        return OTA_ERR_TIMEOUT;
    }

    ota_flash_unlock();

    FLASH->CR &= ~FLASH_CR_SER;
    FLASH->CR |= FLASH_CR_SER;
    FLASH->CR &= ~FLASH_CR_SNB;
    FLASH->CR |= (sector << FLASH_CR_SNB_Pos) & FLASH_CR_SNB;
    FLASH->CR |= FLASH_CR_STRT;

    if (!ota_flash_wait_idle()) {
        ota_flash_lock();
        return OTA_ERR_TIMEOUT;
    }
    /* 擦除失败标志检查，并清EOP避免误判 */
    if ((FLASH->SR & OTA_FLASH_ERR_MASK) != 0u) {
        FLASH->SR = OTA_FLASH_ERR_MASK;
        ota_flash_lock();
        return OTA_ERR_FLASH;
    }
    if ((FLASH->SR & FLASH_SR_EOP) != 0u) {
        FLASH->SR = FLASH_SR_EOP;
    }

    ota_flash_lock();
    return OTA_ERR_OK;
}

/* 擦除[addr, addr+size)覆盖的所有扇区。逐个扇区擦除并检查结果。 */
static int ota_flash_erase_raw(uint32_t addr, uint32_t size)
{
    uint32_t end_addr = addr + size;
    uint32_t i;

    if (!ota_power_ok()) {
        return OTA_ERR_POWER_FAIL;
    }
    if (size == 0u || end_addr <= addr) {
        return OTA_ERR_PARAM;
    }

    for (i = 0; i < OTA_SECTOR_COUNT; i++) {
        uint32_t s_addr = s_sector_table[i].addr;
        uint32_t s_size = s_sector_table[i].size;
        int ret;

        /* 仅擦除与目标区间相交的扇区 */
        if (s_addr < end_addr && (s_addr + s_size) > addr) {
            ret = ota_flash_erase_sector(s_sector_table[i].sector);
            if (ret != OTA_ERR_OK) {
                return ret;
            }
        }
    }

    return OTA_ERR_OK;
}

/* 写入len字节到addr（addr必须4字节对齐，len不足4字节补0xFF）。
 * 使用WORD编程，逐4字节写入并检查错误。 */
static int ota_flash_write_raw(uint32_t addr, const uint8_t *data, size_t len)
{
    uint8_t aligned_buf[4];
    size_t i;

    if (data == NULL || len == 0u) {
        return OTA_ERR_PARAM;
    }
    if ((addr & 0x03u) != 0u) {
        return OTA_ERR_PARAM;
    }
    if (!ota_power_ok()) {
        return OTA_ERR_POWER_FAIL;
    }
    if (!ota_flash_wait_idle()) {
        return OTA_ERR_TIMEOUT;
    }

    ota_flash_unlock();
    FLASH->CR |= FLASH_CR_PG;

    for (i = 0; i < len; i += 4) {
        uint32_t word;
        size_t remain = len - i;

        if (remain >= 4) {
            memcpy(aligned_buf, &data[i], 4);
        } else {
            /* 末尾不足4字节：拷贝有效数据后补0xFF */
            memset(aligned_buf, 0xFF, sizeof(aligned_buf));
            memcpy(aligned_buf, &data[i], remain);
        }
        memcpy(&word, aligned_buf, sizeof(word));

        *(volatile uint32_t *)(addr + i) = word;

        /* 等待编程完成，同时检查编程错误 */
        if (!ota_flash_wait_idle() ||
            (FLASH->SR & OTA_FLASH_ERR_MASK) != 0u) {
            FLASH->CR &= ~FLASH_CR_PG;
            ota_flash_lock();
            return OTA_ERR_FLASH;
        }
    }

    FLASH->CR &= ~FLASH_CR_PG;
    ota_flash_lock();
    return OTA_ERR_OK;
}

/* --------------------------- 固件区读写 --------------------------- */

int ota_flash_erase_app(ota_slot_t slot)
{
    uint32_t addr = ota_flash_app_addr(slot);
    uint32_t size = ota_flash_app_size(slot);
    return ota_flash_erase_raw(addr, size);
}

int ota_flash_write_chunk(ota_slot_t slot, uint32_t offset,
                          const uint8_t *data, size_t len)
{
    uint32_t addr;
    uint32_t size;

    if (data == NULL || len == 0u) {
        return OTA_ERR_PARAM;
    }

    addr = ota_flash_app_addr(slot);
    size = ota_flash_app_size(slot);

    /* 防止越界写坏分区外数据（如参数区） */
    if (offset >= size || len > (size - offset)) {
        return OTA_ERR_PARAM;
    }

    return ota_flash_write_raw(addr + offset, data, len);
}

int ota_flash_read_app(ota_slot_t slot, uint32_t offset, uint8_t *buf, size_t len)
{
    uint32_t addr;
    uint32_t size;

    if (buf == NULL || len == 0u) {
        return OTA_ERR_PARAM;
    }

    addr = ota_flash_app_addr(slot);
    size = ota_flash_app_size(slot);

    if (offset >= size || len > (size - offset)) {
        return OTA_ERR_PARAM;
    }

    memcpy(buf, (const void *)(addr + offset), len);
    return (int)len;
}

/* --------------------------- 参数区读写 --------------------------- */

int ota_flash_write_params(const void *params, size_t len)
{
    int ret;

    if (params == NULL || len == 0u || len > OTA_PARAM_SLOT_SIZE) {
        return OTA_ERR_PARAM;
    }

    /* 擦除整个S11扇区（128KB），此时双槽均为0xFF */
    ret = ota_flash_erase_raw(OTA_PARAM_ADDR, OTA_PARAM_SIZE);
    if (ret != OTA_ERR_OK) {
        return ret;
    }

    /* 依次写两个槽；任一槽成功即可作为有效副本 */
    ret = ota_flash_write_raw(OTA_PARAM_SLOT0_ADDR, (const uint8_t *)params, len);
    if (ret != OTA_ERR_OK) {
        return ret;
    }
    ret = ota_flash_write_raw(OTA_PARAM_SLOT1_ADDR, (const uint8_t *)params, len);
    if (ret != OTA_ERR_OK) {
        return ret;
    }

    return OTA_ERR_OK;
}

int ota_flash_read_param_slot(uint8_t slot, void *params, size_t len)
{
    uint32_t addr;

    if (params == NULL || len == 0u || len > OTA_PARAM_SLOT_SIZE) {
        return OTA_ERR_PARAM;
    }
    if (slot != 0u && slot != 1u) {
        return OTA_ERR_PARAM;
    }

    addr = (slot == 0u) ? OTA_PARAM_SLOT0_ADDR : OTA_PARAM_SLOT1_ADDR;
    memcpy(params, (const void *)addr, len);
    return OTA_ERR_OK;
}
