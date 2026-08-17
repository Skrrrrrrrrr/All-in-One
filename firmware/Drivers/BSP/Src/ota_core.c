/*
 * ota_core.c
 *
 * OTA核心逻辑实现：
 *   - 参数区管理（双槽冗余读/写 + CRC校验）
 *   - A/B下载状态机（begin→write→end→activate）
 *   - 回滚机制（pending_slot + boot_attempts，由Bootloader配合判定）
 *   - vOTATask（静态分配，处理CLI下发的命令）
 *   - 断电保护（写Flash前检查PVDO）
 *
 * 作者: OTA Team
 * 修改记录:
 *   2026-08-07  初始版本
 */

#include "ota_core.h"
#include "stm32f4xx_hal.h"
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "semphr.h"
#include "cmsis_os2.h"
#include "elog.h"
#include <string.h>
#include <stdio.h>

/* ----------------------------- 状态变量 ----------------------------- */
static ota_state_t s_state = OTA_STATE_IDLE;
static ota_slot_t s_target_slot = OTA_SLOT_B;      /* 本次升级目标区 */
static uint32_t s_fw_size = 0;
static uint32_t s_fw_crc32 = 0;
static uint32_t s_fw_version = 0;
static uint32_t s_written = 0;
static ota_params_t s_params;

/* ------------------------- 任务/队列静态资源 ------------------------- */
#define OTA_TASK_STACK_WORDS   (512)
#define OTA_QUEUE_LEN          (8)
#define OTA_CLI_TIMEOUT_MS     (30000u)
#define OTA_WRITE_TIMEOUT_MS   (2000u)
#define OTA_VERIFY_BUF_SIZE    (512u)   /* 校验分块缓冲，避免大数组入栈 */

/* 以下 OTA 任务资源仅被 CPU 访问（无 DMA 参与），全部放入 CCMRAM
 * (0x10000000, 仅 CPU 可访问)。搬移后可释放约 3.2KB 普通 RAM。
 * 注意：.ccmram_bss 为 NOLOAD 段，startup 不清零，已由 freertos.c
 * system_pre_init() 统一清零。 */
#define OTA_CCM_SECTION(sec) __attribute__((section(sec)))

static StackType_t s_ota_stack[OTA_TASK_STACK_WORDS] OTA_CCM_SECTION(".bss.s_ota_stack");
static StaticTask_t s_ota_tcb OTA_CCM_SECTION(".bss.s_ota_tcb");
static StaticQueue_t s_ota_q_cb OTA_CCM_SECTION(".bss.s_ota_q_cb");
static uint8_t s_ota_q_storage[OTA_QUEUE_LEN * 32u] OTA_CCM_SECTION(".bss.s_ota_q_storage");  /* 消息池 */
static QueueHandle_t s_ota_q = NULL;
static StaticSemaphore_t s_ota_done_cb OTA_CCM_SECTION(".bss.s_ota_done_cb");
static SemaphoreHandle_t s_ota_done_sem = NULL;
static volatile int s_ota_result = OTA_ERR_OK;
static uint8_t s_verify_buf[OTA_VERIFY_BUF_SIZE] OTA_CCM_SECTION(".bss.s_verify_buf");

/* 消息定义 */
typedef enum {
    OTA_MSG_BEGIN = 0,
    OTA_MSG_WRITE,
    OTA_MSG_END,
    OTA_MSG_ACTIVATE,
    OTA_MSG_ROLLBACK,
} ota_msg_id_t;

typedef struct {
    ota_msg_id_t id;
    uint32_t arg0;
    uint32_t arg1;
    uint32_t arg2;
    const uint8_t *data;
    uint16_t len;
} ota_msg_t;

/* ----------------------------- 参数管理 ----------------------------- */

static uint32_t params_calc_crc(const ota_params_t *p)
{
    ota_params_t tmp;

    if (p == NULL) {
        return 0u;
    }
    memcpy(&tmp, p, sizeof(tmp));
    tmp.crc32 = 0;   /* 计算时CRC字段清零，避免自引用 */
    return ota_crc32((const uint8_t *)&tmp, sizeof(tmp));
}

static uint8_t params_is_valid(const ota_params_t *p)
{
    if (p == NULL) {
        return 0u;
    }
    if (p->magic != OTA_PARAMS_MAGIC || p->version != OTA_PARAMS_VERSION) {
        return 0u;
    }
    if (p->crc32 != params_calc_crc(p)) {
        return 0u;
    }
    if (p->active_slot != OTA_SLOT_A && p->active_slot != OTA_SLOT_B) {
        return 0u;
    }
    return 1u;
}

static void params_init_default(ota_params_t *p)
{
    if (p == NULL) {
        return;
    }
    memset(p, 0, sizeof(*p));
    p->magic = OTA_PARAMS_MAGIC;
    p->version = OTA_PARAMS_VERSION;
    p->active_slot = OTA_SLOT_A;            /* 默认引导A区 */
    p->pending_slot = OTA_SLOT_INVALID;
    p->boot_attempts = 0;
    p->crc32 = params_calc_crc(p);
}

/* 读有效参数：优先Slot1（后写入），再回退Slot0；全无效返回错误 */
static int params_read(void)
{
    ota_params_t s0, s1;
    int ret;

    ret = ota_flash_read_param_slot(1u, &s1, sizeof(s1));
    if (ret == OTA_ERR_OK && params_is_valid(&s1)) {
        memcpy(&s_params, &s1, sizeof(s_params));
        return OTA_ERR_OK;
    }

    ret = ota_flash_read_param_slot(0u, &s0, sizeof(s0));
    if (ret == OTA_ERR_OK && params_is_valid(&s0)) {
        memcpy(&s_params, &s0, sizeof(s_params));
        return OTA_ERR_OK;
    }

    return OTA_ERR_FLASH;
}

static int params_write(const ota_params_t *p)
{
    if (p == NULL) {
        return OTA_ERR_PARAM;
    }
    return ota_flash_write_params(p, sizeof(*p));
}

/* 判断当前运行区：读VTOR（App部署后指向A或B），无Bootloader阶段按参数 */
static ota_slot_t get_running_slot(void)
{
    uint32_t vtor = SCB->VTOR & 0xFFFFF000u;

    if (vtor == OTA_APP_A_ADDR) {
        return OTA_SLOT_A;
    }
    if (vtor == OTA_APP_B_ADDR) {
        return OTA_SLOT_B;
    }
    return s_params.active_slot;
}

/* ------------------------------- 初始化 ------------------------------- */

void ota_init(void)
{
    if (params_read() != OTA_ERR_OK) {
        /* 首次上电或参数损坏：使用默认参数（引导A区），不自动写回，
         * 避免启动阶段意外擦写Flash。 */
        params_init_default(&s_params);
    }

    s_state = OTA_STATE_IDLE;
    s_written = 0;

    elog_i("OTA", "init done, active slot=%d, pending=%d",
           (int)s_params.active_slot, (int)s_params.pending_slot);
}

ota_state_t ota_get_state(void)
{
    return s_state;
}

ota_slot_t ota_get_active_slot(void)
{
    return s_params.active_slot;
}

ota_slot_t ota_get_target_slot(void)
{
    return s_target_slot;
}

uint32_t ota_get_fw_size(void)
{
    return s_fw_size;
}

uint32_t ota_get_fw_crc32(void)
{
    return s_fw_crc32;
}

uint32_t ota_get_written(void)
{
    return s_written;
}

/* ----------------------------- 升级流程 ----------------------------- */

int ota_begin(uint32_t fw_size, uint32_t fw_crc32, uint32_t fw_version)
{
    int ret;

    if (s_state != OTA_STATE_IDLE) {
        return OTA_ERR_STATE;
    }
    if (fw_size == 0u || fw_size > OTA_FW_MAX_SIZE) {
        return OTA_ERR_SIZE;
    }
    if (!ota_power_ok()) {
        return OTA_ERR_POWER_FAIL;
    }

    /* 目标区 = 当前活跃区的对面 */
    s_target_slot = (s_params.active_slot == OTA_SLOT_A) ? OTA_SLOT_B : OTA_SLOT_A;

    ret = ota_flash_erase_app(s_target_slot);
    if (ret != OTA_ERR_OK) {
        s_state = OTA_STATE_ERROR;
        elog_e("OTA", "begin: erase slot %d failed, ret=%d", (int)s_target_slot, ret);
        return ret;
    }

    s_fw_size = fw_size;
    s_fw_crc32 = fw_crc32;
    s_fw_version = fw_version;
    s_written = 0;
    s_state = OTA_STATE_WRITING;

    elog_i("OTA", "begin: erase slot %d OK, size=%u crc=0x%08X ver=%u",
           (int)s_target_slot, fw_size, fw_crc32, fw_version);
    return OTA_ERR_OK;
}

int ota_write_chunk(uint32_t offset, const uint8_t *data, size_t len)
{
    int ret;
    uint32_t prev_written;

    if (s_state != OTA_STATE_WRITING) {
        return OTA_ERR_STATE;
    }
    if (data == NULL || len == 0u) {
        return OTA_ERR_PARAM;
    }
    /* 强制顺序写，offset必须等于已写字节数，防止乱序/覆盖 */
    if (offset != s_written) {
        return OTA_ERR_PARAM;
    }
    if ((uint64_t)s_written + len > s_fw_size) {
        return OTA_ERR_SIZE;
    }

    ret = ota_flash_write_chunk(s_target_slot, offset, data, len);
    if (ret != OTA_ERR_OK) {
        s_state = OTA_STATE_ERROR;
        elog_e("OTA", "write: fail at off=%u ret=%d", offset, ret);
        return ret;
    }

    prev_written = s_written;
    s_written += (uint32_t)len;

    /* 每64KB输出一次进度日志，便于追溯和观察 */
    if ((s_written / 0x10000u) != (prev_written / 0x10000u)) {
        elog_i("OTA", "write: %u/%u bytes (%u%%)", s_written, s_fw_size,
               (uint32_t)(((uint64_t)s_written * 100u) / s_fw_size));
    }
    return OTA_ERR_OK;
}

int ota_end(void)
{
    uint32_t crc = 0xFFFFFFFFu;
    uint32_t offset = 0;

    if (s_state != OTA_STATE_WRITING) {
        return OTA_ERR_STATE;
    }
    if (s_written != s_fw_size) {
        elog_e("OTA", "end: size mismatch, written=%u expect=%u", s_written, s_fw_size);
        s_state = OTA_STATE_ERROR;
        return OTA_ERR_SIZE;
    }
    if (!ota_power_ok()) {
        s_state = OTA_STATE_ERROR;
        return OTA_ERR_POWER_FAIL;
    }

    s_state = OTA_STATE_VERIFYING;
    elog_i("OTA", "end: verifying CRC32 (size=%u)...", s_fw_size);

    /* 分块读回并增量计算CRC32，避免一次性读入整个固件 */
    while (offset < s_fw_size) {
        uint32_t chunk = (s_fw_size - offset > OTA_VERIFY_BUF_SIZE) ?
                         OTA_VERIFY_BUF_SIZE : (s_fw_size - offset);
        int ret = ota_flash_read_app(s_target_slot, offset, s_verify_buf, chunk);
        if (ret < 0) {
            s_state = OTA_STATE_ERROR;
            elog_e("OTA", "end: read fail at off=%u ret=%d", offset, ret);
            return ret;
        }
        crc = ota_crc32_update(crc, s_verify_buf, chunk);
        offset += chunk;
    }
    crc ^= 0xFFFFFFFFu;

    if (crc != s_fw_crc32) {
        s_state = OTA_STATE_ERROR;
        elog_e("OTA", "end: CRC mismatch, calc=0x%08X expect=0x%08X", crc, s_fw_crc32);
        return OTA_ERR_CRC;
    }

    s_state = OTA_STATE_READY;
    elog_i("OTA", "end: CRC OK (0x%08X), ready to activate", crc);
    return OTA_ERR_OK;
}

int ota_activate(void)
{
    ota_params_t p;

    if (s_state != OTA_STATE_READY) {
        return OTA_ERR_STATE;
    }
    if (!ota_power_ok()) {
        return OTA_ERR_POWER_FAIL;
    }

    /* 记录待验证区；active_slot保持不变（Bootloader按其决定是否切换） */
    p = s_params;
    p.pending_slot = s_target_slot;
    p.boot_attempts = 0;
    p.fw_size = s_fw_size;
    p.fw_crc32 = s_fw_crc32;
    p.fw_version = s_fw_version;
    p.crc32 = params_calc_crc(&p);

    if (params_write(&p) != OTA_ERR_OK) {
        s_state = OTA_STATE_ERROR;
        elog_e("OTA", "activate: write params failed");
        return OTA_ERR_FLASH;
    }

    s_state = OTA_STATE_IDLE;
    elog_i("OTA", "activate: pending slot=%d, reboot to apply",
           (int)s_target_slot);
    return OTA_ERR_OK;
}

int ota_rollback(void)
{
    ota_params_t p;

    if (params_read() != OTA_ERR_OK) {
        return OTA_ERR_FLASH;
    }
    p = s_params;
    p.pending_slot = OTA_SLOT_INVALID;
    p.boot_attempts = 0;
    p.fw_size = 0;
    p.fw_crc32 = 0;
    p.fw_version = 0;
    p.crc32 = params_calc_crc(&p);

    if (params_write(&p) != OTA_ERR_OK) {
        elog_e("OTA", "rollback: write params failed");
        return OTA_ERR_FLASH;
    }

    elog_w("OTA", "rollback: force boot from slot %d", (int)p.active_slot);
    return OTA_ERR_OK;
}

/* App运行正常后调用，确认当前区，清除待验证状态 */
void ota_confirm_ok(void)
{
    ota_params_t p;
    ota_slot_t running;

    if (params_read() != OTA_ERR_OK) {
        return;
    }
    running = get_running_slot();

    p = s_params;
    p.active_slot = running;
    p.pending_slot = OTA_SLOT_INVALID;
    p.boot_attempts = 0;
    p.fw_size = 0;
    p.fw_crc32 = 0;
    p.fw_version = 0;
    p.crc32 = params_calc_crc(&p);

    if (params_write(&p) == OTA_ERR_OK) {
        elog_i("OTA", "confirm: running slot=%d confirmed OK", (int)running);
    } else {
        elog_e("OTA", "confirm: write params failed");
    }
}

/* --------------------------- 状态字符串 --------------------------- */

void ota_get_status_string(char *buf, size_t len)
{
    static const char *state_str[] = {
        "IDLE", "WRITING", "VERIFYING", "READY", "ABORTED", "ERROR"
    };
    const char *sname;

    if (buf == NULL || len == 0u) {
        return;
    }

    if ((uint32_t)s_state >= sizeof(state_str) / sizeof(state_str[0])) {
        sname = "UNKNOWN";
    } else {
        sname = state_str[s_state];
    }

    snprintf(buf, len,
             "\r\n--- OTA Status ---\r\n"
             "State: %s\r\n"
             "Active slot: %s\r\n"
             "Target slot: %s\r\n"
             "Firmware: %u bytes, CRC 0x%08X, ver %u\r\n"
             "Written: %u bytes\r\n",
             sname,
             (s_params.active_slot == OTA_SLOT_A) ? "A" : "B",
             (s_target_slot == OTA_SLOT_A) ? "A" : "B",
             (unsigned int)s_fw_size, (unsigned int)s_fw_crc32,
             (unsigned int)s_fw_version, (unsigned int)s_written);
}

/* ------------------------- vOTATask + CLI接口 ------------------------- */

static void vOTATask(void *argument)
{
    ota_msg_t msg;

    (void)argument;

    for (;;) {
        /* 阻塞等待CLI下发的命令 */
        if (xQueueReceive(s_ota_q, &msg, portMAX_DELAY) == pdTRUE) {
            int result = OTA_ERR_PARAM;

            switch (msg.id) {
                case OTA_MSG_BEGIN:
                    result = ota_begin(msg.arg0, msg.arg1, msg.arg2);
                    break;
                case OTA_MSG_WRITE:
                    result = ota_write_chunk(msg.arg0, msg.data, msg.len);
                    break;
                case OTA_MSG_END:
                    result = ota_end();
                    break;
                case OTA_MSG_ACTIVATE:
                    result = ota_activate();
                    break;
                case OTA_MSG_ROLLBACK:
                    result = ota_rollback();
                    break;
                default:
                    break;
            }

            /* 回传结果，唤醒等待中的CLI任务 */
            s_ota_result = result;
            xSemaphoreGive(s_ota_done_sem);
        }
    }
}

int ota_task_start(void)
{
    osThreadAttr_t attr;

    if (s_ota_q != NULL) {
        return OTA_ERR_OK;   /* 已启动 */
    }

    s_ota_q = xQueueCreateStatic(OTA_QUEUE_LEN, sizeof(ota_msg_t),
                                 s_ota_q_storage, &s_ota_q_cb);
    if (s_ota_q == NULL) {
        return OTA_ERR_STATE;
    }

    s_ota_done_sem = xSemaphoreCreateBinaryStatic(&s_ota_done_cb);
    if (s_ota_done_sem == NULL) {
        return OTA_ERR_STATE;
    }

    /* 静态创建vOTATask，避免动态堆耗尽（堆仅32KB，LwIP教训） */
    memset(&attr, 0, sizeof(attr));
    attr.name = "vOTATask";
    attr.cb_mem = &s_ota_tcb;
    attr.cb_size = sizeof(StaticTask_t);
    attr.stack_mem = s_ota_stack;
    attr.stack_size = sizeof(s_ota_stack);
    /* 优先级必须低于myTask(osPriorityLow)：vOTATask执行Flash擦写/校验时是
     * 忙等（HAL轮询BSY，不进入阻塞），若其优先级更高，会饿死myTask，
     * 导致PVD轮询(pvd_poll_handler, ~1ms)被延迟，掉电确认/重启流程可能
     * 赶不上保持电容(约5-10ms)耗尽。vOTATask平时阻塞在队列上，几乎不占
     * CPU，用osPriorityIdle完全满足实时性；Flash写期间被myTask抢占正是
     * 需要的效果。 */
    attr.priority = osPriorityIdle;

    if (osThreadNew(vOTATask, NULL, &attr) == NULL) {
        return OTA_ERR_STATE;
    }

    elog_i("OTA", "vOTATask started (static, prio=normal)");
    return OTA_ERR_OK;
}

static int ota_cli_request(const ota_msg_t *msg, uint32_t timeout_ms)
{
    TickType_t ticks;

    if (msg == NULL || s_ota_q == NULL || s_ota_done_sem == NULL) {
        return OTA_ERR_STATE;
    }

    ticks = pdMS_TO_TICKS(timeout_ms);

    if (xQueueSend(s_ota_q, msg, ticks) != pdPASS) {
        return OTA_ERR_TIMEOUT;
    }
    /* 等待vOTATask执行完成并回传结果 */
    if (xSemaphoreTake(s_ota_done_sem, ticks) != pdPASS) {
        return OTA_ERR_TIMEOUT;
    }
    return s_ota_result;
}

int ota_cli_begin(uint32_t size, uint32_t crc, uint32_t version)
{
    ota_msg_t m;
    memset(&m, 0, sizeof(m));
    m.id = OTA_MSG_BEGIN;
    m.arg0 = size;
    m.arg1 = crc;
    m.arg2 = version;
    return ota_cli_request(&m, OTA_CLI_TIMEOUT_MS);
}

int ota_cli_write(uint32_t offset, const uint8_t *data, uint16_t len)
{
    ota_msg_t m;
    memset(&m, 0, sizeof(m));
    m.id = OTA_MSG_WRITE;
    m.arg0 = offset;
    m.data = data;
    m.len = len;
    return ota_cli_request(&m, OTA_WRITE_TIMEOUT_MS);
}

int ota_cli_end(void)
{
    ota_msg_t m;
    memset(&m, 0, sizeof(m));
    m.id = OTA_MSG_END;
    return ota_cli_request(&m, OTA_CLI_TIMEOUT_MS);
}

int ota_cli_activate(void)
{
    ota_msg_t m;
    memset(&m, 0, sizeof(m));
    m.id = OTA_MSG_ACTIVATE;
    return ota_cli_request(&m, OTA_CLI_TIMEOUT_MS);
}

int ota_cli_rollback(void)
{
    ota_msg_t m;
    memset(&m, 0, sizeof(m));
    m.id = OTA_MSG_ROLLBACK;
    return ota_cli_request(&m, OTA_CLI_TIMEOUT_MS);
}

int ota_cli_confirm(void)
{
    /* confirm为轻量操作，由CLI任务直接执行（无需排队） */
    ota_confirm_ok();
    return OTA_ERR_OK;
}
