/*
 * bootloader_reference.c
 *
 * Bootloader引导/回滚逻辑参考实现（STM32F407ZGTx，内部Flash 1MB）。
 *
 * 重要说明：
 *   1. 本文件是【独立参考工程】，不参与App编译，不依赖FreeRTOS。
 *      部署时请将其作为独立Bootloader工程源码，与 ota_flash_port.c 一起编译
 *      （ota_flash_port.c 仅依赖HAL，无RTOS依赖）。
 *   2. 参数结构与校验算法必须与 ota_core.c 中的 ota_params_t 严格一致，
 *      否则App写入的启动标志Bootloader无法识别。
 *   3. 本文件使用寄存器级Flash操作（Bootloader阶段HAL可能未初始化），
 *      逻辑与 ota_flash_port.c 的HAL实现等价。
 *
 * 引导流程：
 *   bootloader_entry()
 *     → 读参数（双槽冗余，Slot1优先，magic+CRC32校验）
 *     → 无pending：直接引导 active_slot
 *     → 有pending（新固件待验证）：
 *         boot_attempts < 3 → boot_attempts++，写回，引导 pending_slot
 *         boot_attempts >= 3 → 判定新固件启动失败，回滚引导 active_slot
 *     → 目标App向量表非法 → 尝试另一区 → 全部非法 → 死循环等待（防变砖底线）
 *
 * 作者: OTA Team
 * 修改记录:
 *   2026-08-07  初始版本
 */

#include "ota_flash_port.h"
#include "stm32f4xx_hal.h"   /* FLASH寄存器、SCB->VTOR、__disable_irq/__set_MSP */
#include <stdint.h>
#include <string.h>

/* ------------------- 必须与 ota_core.h 中 ota_params_t 一致 ------------------- */
#define BL_PARAMS_MAGIC        (0x4F54414Du)   /* "OTAM" */
#define BL_PARAMS_VERSION      (1u)
#define BL_SLOT_INVALID        (0xFFu)
#define BL_MAX_BOOT_ATTEMPTS   (3u)            /* 新固件最大启动尝试次数 */

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t crc32;         /* 校验从magic到reserved2（计算时crc32置0） */
    uint8_t  active_slot;   /* 当前引导区（0=A, 1=B） */
    uint8_t  pending_slot;  /* 待验证区（BL_SLOT_INVALID=无） */
    uint8_t  boot_attempts; /* 新固件已尝试启动次数 */
    uint8_t  reserved1;
    uint32_t fw_size;
    uint32_t fw_crc32;
    uint32_t fw_version;
    uint32_t reserved2;
} bl_params_t;              /* 40字节，4字节对齐 */

/* ----------------------------- 寄存器级Flash操作 ----------------------------- */
/* 说明：与 ota_flash_port.c 的寄存器级实现逻辑等价，Bootloader工程亦可直接
 * 调用 ota_flash_port.c 提供的 ota_flash_write_params/ota_flash_erase_app
 * 等接口（该文件无RTOS依赖）。 */

#define BL_CORE_FREQ_HZ           (168000000u)  /* F407最高主频 */
/* 等待Flash空闲超时：168MHz×5s = 840M周期，覆盖最大128KB扇区擦除(约1~2s) */
#define BL_FLASH_TIMEOUT_CYCLES   ((uint32_t)(BL_CORE_FREQ_HZ * 5u))
/* 编程/擦除错误标志组合（旧版CMSIS头文件无FLASH_SR_PGERR，逐位列出） */
#define BL_FLASH_ERR_MASK         (FLASH_SR_WRPERR | FLASH_SR_PGAERR | \
                                   FLASH_SR_PGPERR | FLASH_SR_PGSERR)

static void bl_flash_wait_init(void)
{
    /* 惰性使能DWT周期计数器（Cortex-M4内置，无需外设）。
     * CYCCNT为32位，@168MHz约25.5s回绕，5s超时内差值运算安全。 */
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0u;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
}

/* 等待Flash空闲，带超时；超时返回0 */
static uint8_t bl_flash_wait_idle(void)
{
    uint32_t start;

    if ((CoreDebug->DEMCR & CoreDebug_DEMCR_TRCENA_Msk) == 0u) {
        bl_flash_wait_init();
    }
    start = DWT->CYCCNT;

    while ((FLASH->SR & FLASH_SR_BSY) != 0u) {
        /* 无符号差值，CYCCNT回绕时依然正确 */
        if ((DWT->CYCCNT - start) >= BL_FLASH_TIMEOUT_CYCLES) {
            return 0u;
        }
    }
    return 1u;
}

/* 解锁Flash（写两个密钥寄存器） */
static void bl_flash_unlock(void)
{
    FLASH->KEYR = 0x45670123u;
    FLASH->KEYR = 0xCDEF89ABu;
}

static void bl_flash_lock(void)
{
    FLASH->CR |= FLASH_CR_LOCK;
}

/* 擦除单个扇区（sector: 0~11），带超时 */
static int bl_flash_erase_sector(uint32_t sector)
{
    if (sector > 11u) {
        return OTA_ERR_PARAM;
    }
    if (bl_flash_wait_idle() == 0u) {
        return OTA_ERR_TIMEOUT;
    }

    bl_flash_unlock();

    FLASH->CR &= ~FLASH_CR_SER;
    FLASH->CR |= FLASH_CR_SER;
    FLASH->CR &= ~(FLASH_CR_SNB);
    FLASH->CR |= (sector << FLASH_CR_SNB_Pos) & FLASH_CR_SNB;
    FLASH->CR |= FLASH_CR_STRT;

    if (bl_flash_wait_idle() == 0u) {
        bl_flash_lock();
        return OTA_ERR_TIMEOUT;
    }
    /* 擦除完成后清EOP，避免后续编程误判 */
    if ((FLASH->SR & FLASH_SR_EOP) != 0u) {
        FLASH->SR = FLASH_SR_EOP;
    }

    bl_flash_lock();
    return OTA_ERR_OK;
}

/* WORD编程len字节到addr（addr必须4字节对齐），带超时 */
static int bl_flash_write_raw(uint32_t addr, const uint8_t *data, size_t len)
{
    uint8_t  aligned_buf[4];
    uint32_t word;
    size_t   i;

    if (data == NULL || len == 0u) {
        return OTA_ERR_PARAM;
    }
    if ((addr & 0x03u) != 0u) {
        return OTA_ERR_PARAM;
    }

    bl_flash_unlock();
    FLASH->CR |= FLASH_CR_PG;

    for (i = 0; i < len; i += 4) {
        size_t remain = len - i;

        if (remain >= 4u) {
            memcpy(aligned_buf, &data[i], 4u);
        } else {
            memset(aligned_buf, 0xFF, sizeof(aligned_buf));
            memcpy(aligned_buf, &data[i], remain);
        }
        memcpy(&word, aligned_buf, sizeof(word));

        /* 写入地址必须对齐到4字节 */
        *(volatile uint32_t *)(addr + i) = word;

        /* 等待编程完成；同时检查编程错误 */
        if (bl_flash_wait_idle() == 0u ||
            (FLASH->SR & BL_FLASH_ERR_MASK) != 0u) {
            FLASH->CR &= ~FLASH_CR_PG;
            bl_flash_lock();
            return OTA_ERR_FLASH;
        }
    }

    FLASH->CR &= ~FLASH_CR_PG;
    bl_flash_lock();
    return OTA_ERR_OK;
}

/* 擦除参数区整个扇区S11（128KB），双槽均回到0xFF */
static int bl_flash_erase_param_sector(void)
{
    return bl_flash_erase_sector(11u);
}

/* ----------------------------- 参数读写（与ota_core.c一致） ----------------------------- */

static uint32_t bl_params_calc_crc(const bl_params_t *p)
{
    bl_params_t tmp;

    if (p == NULL) {
        return 0u;
    }
    memcpy(&tmp, p, sizeof(tmp));
    tmp.crc32 = 0u;   /* 计算时CRC字段清零，避免自引用 */
    return ota_crc32((const uint8_t *)&tmp, sizeof(tmp));
}

static uint8_t bl_params_is_valid(const bl_params_t *p)
{
    if (p == NULL) {
        return 0u;
    }
    if (p->magic != BL_PARAMS_MAGIC || p->version != BL_PARAMS_VERSION) {
        return 0u;
    }
    if (p->crc32 != bl_params_calc_crc(p)) {
        return 0u;
    }
    if (p->active_slot != OTA_SLOT_A && p->active_slot != OTA_SLOT_B) {
        return 0u;
    }
    return 1u;
}

/* 读参数：优先Slot1（后写入，较新），再回退Slot0；全无效返回错误 */
static int bl_params_read(bl_params_t *p)
{
    bl_params_t s0;
    bl_params_t s1;

    if (p == NULL) {
        return OTA_ERR_PARAM;
    }

    /* 内部Flash支持直接指针读取，无需解锁 */
    memcpy(&s1, (const void *)OTA_PARAM_SLOT1_ADDR, sizeof(s1));
    if (bl_params_is_valid(&s1)) {
        *p = s1;
        return OTA_ERR_OK;
    }

    memcpy(&s0, (const void *)OTA_PARAM_SLOT0_ADDR, sizeof(s0));
    if (bl_params_is_valid(&s0)) {
        *p = s0;
        return OTA_ERR_OK;
    }

    return OTA_ERR_FLASH;
}

/* 写参数：擦除S11 → 写Slot0 → 写Slot1（与ota_core.c的params_write等价） */
static int bl_params_write(const bl_params_t *p)
{
    int ret;

    if (p == NULL) {
        return OTA_ERR_PARAM;
    }

    ret = bl_flash_erase_param_sector();
    if (ret != OTA_ERR_OK) {
        return ret;
    }

    ret = bl_flash_write_raw(OTA_PARAM_SLOT0_ADDR, (const uint8_t *)p, sizeof(*p));
    if (ret != OTA_ERR_OK) {
        return ret;
    }

    ret = bl_flash_write_raw(OTA_PARAM_SLOT1_ADDR, (const uint8_t *)p, sizeof(*p));
    if (ret != OTA_ERR_OK) {
        return ret;
    }

    return OTA_ERR_OK;
}

/* ----------------------------- App镜像合法性校验 ----------------------------- */

/* 校验App向量表合法性：
 *   - 栈顶指针必须落在SRAM（0x20000000 ~ 0x20020000）
 *   - Reset Handler必须落在内部Flash（0x08000000 ~ 0x08100000）
 * 这两条可过滤绝大多数的无效/擦除后的镜像，防止跳转后HardFault。 */
static uint8_t bl_app_valid(uint32_t app_addr)
{
    uint32_t msp;
    uint32_t reset_handler;

    if (app_addr < 0x08000000u || app_addr >= 0x08100000u) {
        return 0u;
    }

    msp = *(volatile uint32_t *)app_addr;
    reset_handler = *(volatile uint32_t *)(app_addr + 4u);

    if (msp < 0x20000000u || msp >= 0x20020000u) {
        return 0u;
    }
    if (reset_handler < 0x08000000u || reset_handler >= 0x08100000u) {
        return 0u;
    }
    return 1u;
}

/* ----------------------------- 跳转App ----------------------------- */

/* 跳转到指定分区App。关中断 → 重定位VTOR → 设置MSP → 跳转。 */
static void bl_jump_to_app(uint32_t app_addr)
{
    typedef void (*app_entry_t)(void);
    app_entry_t app_entry;

    __disable_irq();

    /* 重定位中断向量表到App所在分区（关键：App中断才能正常响应） */
    SCB->VTOR = app_addr;

    /* 取App向量表的栈顶指针并设置MSP */
    __set_MSP(*(volatile uint32_t *)app_addr);

    /* 取Reset Handler地址（Thumb位已含在地址最低位） */
    app_entry = (app_entry_t)(*(volatile uint32_t *)(app_addr + 4u));

    app_entry();

    /* 正常不会返回；若返回则说明App非法，死循环保护 */
    for (;;) {
    }
}

/* ----------------------------- 引导入口 ----------------------------- */

/* 引导入口：上电后由启动代码调用。
 * 返回前要么已跳转App，要么死循环（无有效App）。 */
void bootloader_entry(void)
{
    bl_params_t params;
    uint32_t    boot_addr;
    ota_slot_t  boot_slot;

    /* 默认引导A区（出厂/参数损坏时的底线） */
    memset(&params, 0, sizeof(params));
    params.magic = BL_PARAMS_MAGIC;
    params.version = BL_PARAMS_VERSION;
    params.active_slot = OTA_SLOT_A;
    params.pending_slot = BL_SLOT_INVALID;

    if (bl_params_read(&params) != OTA_ERR_OK) {
        /* 参数区无效（首次上电或断电损坏）：默认引导A区，并回写一份
         * 有效参数，使后续OTA流程可用。 */
        params.crc32 = bl_params_calc_crc(&params);
        (void)bl_params_write(&params);
    }

    /* 有待验证固件（pending_slot）时，按尝试次数决定引导或回滚 */
    if (params.pending_slot != BL_SLOT_INVALID) {
        if (params.boot_attempts < BL_MAX_BOOT_ATTEMPTS) {
            /* 新固件尝试次数未超限：尝试次数+1后引导新固件 */
            params.boot_attempts++;
            params.crc32 = bl_params_calc_crc(&params);
            (void)bl_params_write(&params);
            boot_slot = (ota_slot_t)params.pending_slot;
        } else {
            /* 新固件连续启动失败超限：回滚到旧活跃区。
             * 清除pending，下次上电直接引导active_slot。 */
            params.pending_slot = BL_SLOT_INVALID;
            params.boot_attempts = 0u;
            params.fw_size = 0u;
            params.fw_crc32 = 0u;
            params.fw_version = 0u;
            params.crc32 = bl_params_calc_crc(&params);
            (void)bl_params_write(&params);
            boot_slot = (ota_slot_t)params.active_slot;
        }
    } else {
        boot_slot = (ota_slot_t)params.active_slot;
    }

    boot_addr = ota_flash_app_addr(boot_slot);

    /* 目标镜像非法（擦除中断/写入损坏）时回退另一区，双保险防变砖 */
    if (!bl_app_valid(boot_addr)) {
        boot_addr = (boot_slot == OTA_SLOT_A) ?
                    ota_flash_app_addr(OTA_SLOT_B) : ota_flash_app_addr(OTA_SLOT_A);
        if (!bl_app_valid(boot_addr)) {
            /* 双区均无效：死循环等待恢复（串口可接Bootloader升级通道） */
            for (;;) {
            }
        }
    }

    bl_jump_to_app(boot_addr);
}
