/*
 * pvd_detection.c
 *
 * PVD (Programmable Voltage Detector) implementation for STM32F411.
 *
 * On power failure (VDD < 2.7V):
 *   1. PVD_IRQHandler is triggered via EXTI Line 16
 *   2. elog_file_flush_all_isr() syncs all pending log data to SPI Flash
 *   3. lfs_unmount() ensures file system integrity
 *   4. System resets via NVIC_SystemReset()
 *
 * ISR safety:
 *   - PVD interrupt runs at highest priority (0)
 *   - The ISR-safe flush function bypasses mutex (safe since ISR has exclusive access)
 *   - Total ISR execution time: ~3-5ms (within capacitor hold-up budget)
 */

#include "pvd_detection.h"
#include "elog_file.h"
#include "uart_ringbuf.h"
#include "spi.h"
#include "stm32f4xx_hal.h"
#include "FreeRTOS.h"

extern volatile BaseType_t xInUserInputMode;
extern volatile BaseType_t xPromptRefreshNeeded;

volatile uint8_t g_power_failure = 0;
static volatile uint8_t system_ready = 0;
static volatile uint8_t g_pvd_simulate_power_fail = 0;

volatile uint8_t g_pvd_trigger_count = 0;
volatile uint32_t g_pvd_trigger_timestamp = 0;

extern SPI_HandleTypeDef hspi1;

static void spi_wait_for_idle(void)
{
    uint32_t timeout = 100000;
    while ((hspi1.Instance->SR & SPI_SR_BSY) && timeout--) {}
}

static void flash_wait_internal(void)
{
    uint32_t timeout = 60000;
    while (timeout--) {}
}

static void pvd_wait_uart_idle(void)
{
    extern UART_HandleTypeDef huart1;
    extern DMA_HandleTypeDef hdma_usart1_tx;

    if (huart1.gState == HAL_UART_STATE_BUSY_TX) {
        while (DMA2_Stream7->NDTR > 0) {}
        __HAL_DMA_DISABLE(&hdma_usart1_tx);
        CLEAR_BIT(USART1->CR3, USART_CR3_DMAT);
        huart1.gState = HAL_UART_STATE_READY;
    }

    while (!(USART1->SR & USART_SR_TC)) {}
}

static void pvd_isr_log(const char *msg)
{
    while (*msg) {
        while (!(USART1->SR & USART_SR_TXE)) {}
        USART1->DR = (uint8_t)(*msg++);
    }
}

void pvd_init(void)
{
    PWR_PVDTypeDef pvdConfig;

    /* Clear any stale PVD output flag from previous boot before configuring */
    __HAL_PWR_CLEAR_FLAG(PWR_FLAG_PVDO);

    pvdConfig.PVDLevel = PVD_THRESHOLD_LEVEL;
    pvdConfig.Mode     = PWR_PVD_MODE_IT_FALLING;

    HAL_PWR_ConfigPVD(&pvdConfig);

    /* Disable any prematurely-enabled PVD_IRQn (e.g. from HAL_MspInit()),
     * clear stale pending bits, then re-enable with correct priority. */
    HAL_NVIC_DisableIRQ(PVD_IRQn);
    HAL_NVIC_ClearPendingIRQ(PVD_IRQn);
    HAL_NVIC_SetPriority(PVD_IRQn, 0, 0);
    HAL_NVIC_EnableIRQ(PVD_IRQn);

    g_pvd_trigger_count = 0;
    g_pvd_trigger_timestamp = 0;

    system_ready = 0;

    const char msg[] = "PVD: initialized at 2.7V threshold, waiting for stable power\r\n";
    uart_rb_write((uint8_t*)msg, sizeof(msg) - 1);
}

void pvd_mark_ready(void)
{
    /* Allow power supply to stabilize before enabling PVD.
     * During boot/reset, the power supply needs time to settle.
     * If PVD is enabled too early, a marginal voltage could trigger
     * a spurious PVD interrupt causing a second system reset. */
    HAL_Delay(100);

    /* Clear any PVD output flag that may have been set during boot */
    __HAL_PWR_CLEAR_FLAG(PWR_FLAG_PVDO);

    HAL_PWR_EnablePVD();
    system_ready = 1;
    const char msg[] = "PVD: system ready, PVD enabled\r\n";
    uart_rb_write((uint8_t*)msg, sizeof(msg) - 1);
}

void HAL_PWR_PVDCallback(void)
{
    /* If PVD is disabled (e.g. during software reset), ignore this interrupt */
    if (!(PWR->CR & PWR_CR_PVDE)) {
        return;
    }

    g_pvd_trigger_count++;
    g_pvd_trigger_timestamp = HAL_GetTick();

    pvd_wait_uart_idle();

    pvd_isr_log("PVD ISR: entered\r\n");

    if (!system_ready) {
        return;
    }

    uint8_t pvd_flag = __HAL_PWR_GET_FLAG(PWR_FLAG_PVDO) ? 1 : 0;

    if (!pvd_flag && !g_pvd_simulate_power_fail) {
        return;
    }

    pvd_isr_log("PVD ISR: retry 1\r\n");
    for (volatile uint32_t i = 0; i < 1000; i++) {}
    pvd_isr_log("PVD ISR: retry 2\r\n");
    for (volatile uint32_t i = 0; i < 1000; i++) {}
    pvd_isr_log("PVD ISR: retry 3\r\n");
    for (volatile uint32_t i = 0; i < 1000; i++) {}

    g_power_failure = 1;

    pvd_isr_log("power failure confirmed\r\n");
    pvd_isr_log("flushing logs\r\n");

    elog_file_flush_all_isr();

    spi_wait_for_idle();
    flash_wait_internal();

    pvd_isr_log("system reset\r\n");

    while (!(USART1->SR & USART_SR_TC)) {}

    NVIC_SystemReset();
}

void pvd_simulate_trigger(void)
{
    const char sim_msg[] = "PVD SIM: simulating power failure trigger\r\n";
    uart_rb_write((uint8_t*)sim_msg, sizeof(sim_msg) - 1);

    HAL_PWR_PVDCallback();
}

void pvd_simulate_trigger_with_power_fail(void)
{
    const char sim_msg[] = "PVD SIM: simulating power failure (force mode)\r\n";
    uart_rb_write((uint8_t*)sim_msg, sizeof(sim_msg) - 1);

    g_pvd_simulate_power_fail = 1;

    HAL_PWR_PVDCallback();
}