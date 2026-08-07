/*
 * pvd_detection.c
 *
 * PVD (Programmable Voltage Detector) implementation for STM32F407.
 *
 * Architecture (two-stage, soft-delay monitoring):
 *   1. HAL_PWR_PVDCallback (ISR context) - LIGHTWEIGHT:
 *        Records count/timestamp, sets g_pvd_event_flag, clears EXTI pending.
 *        No busy-waits, no FreeRTOS API calls, no SPI/Flash access.
 *        Exits in < 1us to avoid blocking lower-prio IRQs.
 *
 *   2. pvd_poll_handler (task context, called from StartMyTask every ~1ms):
 *        When g_pvd_event_flag is set, runs a soft-delay PVDO verification
 *        window of ~3-5ms using vTaskDelay (yields CPU, no DWT busy-wait):
 *          check 1 (t=0)    -> read PVDO -> log "retry N"
 *          vTaskDelay(2ms)  -> t~2ms
 *          check 2 (t~2ms)  -> read PVDO -> log "retry N+1"
 *          vTaskDelay(2ms)  -> t~4ms
 *          check 3 (t~4ms)  -> read PVDO -> log "retry N+2"
 *        Retry rounds are bound to g_pvd_trigger_count (start counter N).
 *        All 3 checks must see PVDO=1 to confirm power failure. If any
 *        check reads PVDO=0, voltage recovered, abort and clear state.
 *        After each monitoring round, g_pvd_event_flag and
 *        g_pvd_trigger_count are cleared.
 *
 * On real power failure (VDD < 2.7V):
 *   - Hold-up capacitor (>= 100uF) gives ~5-10ms of budget.
 *   - myTask wakes every ~1ms; worst-case event-to-detect latency is
 *     ~1ms poll + ~4ms window = ~5ms.
 *   - The soft-delay window filters out transient glitches.
 */

#include "pvd_detection.h"
#include "elog_file.h"
#include "uart_ringbuf.h"
#include "spi.h"
#include "stm32f4xx_hal.h"
#include "FreeRTOS.h"
#include "task.h"
#include <stdio.h>

volatile uint8_t g_power_failure = 0;
volatile uint8_t g_pvd_trigger_count = 0;
volatile uint32_t g_pvd_trigger_timestamp = 0;

static volatile uint8_t system_ready = 0;
static volatile uint8_t g_pvd_event_flag = 0;   /* set by ISR, cleared by handler */
static const uint8_t PVD_ISR_RETRY_MAX_COUNT = 10;/* 10ms PVD detect*/
extern SPI_HandleTypeDef hspi1;

#if (!ELOG_FILE_SYNC_ON_WRITE)
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
#endif


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

static void pvd_direct_log(const char *msg)
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

    /* Reconfigure PVD IRQ: clear stale pending bits and use a priority
     * that does not need FreeRTOS API calls (the ISR only sets a flag). */
    HAL_NVIC_DisableIRQ(PVD_IRQn);
    HAL_NVIC_ClearPendingIRQ(PVD_IRQn);
    HAL_NVIC_SetPriority(PVD_IRQn, 5, 0);
    HAL_NVIC_EnableIRQ(PVD_IRQn);

    g_pvd_trigger_count = 0;
    g_pvd_event_flag = 0;
    system_ready = 0;

    const char msg[] = "[PVD] initialized at 2.7V threshold, waiting for stable power\r\n";
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
    const char msg[] = "[PVD] system ready, PVD enabled\r\n";
    uart_rb_write((uint8_t*)msg, sizeof(msg) - 1);
}

/* ------------------------------------------------------------------
 * ISR: Minimal action - only record the event, exit immediately.
 * All verification / shutdown logic lives in pvd_poll_handler
 * which runs in task context (StartMyTask soft-delay loop).
 * ------------------------------------------------------------------ */
void HAL_PWR_PVDCallback(void)
{
    /* If PVD peripheral itself is disabled (e.g. during software reset),
     * this must be a stale interrupt - ignore it. */
    if (!(PWR->CR & PWR_CR_PVDE)) {
        return;
    }

    /* System not yet initialized - just drop the event */
    if (!system_ready) {
        return;
    }

    g_pvd_trigger_count++;
    g_pvd_trigger_timestamp = HAL_GetTick();

    /* Signal the event to the task-context handler */
    g_pvd_event_flag = 1;

    /* Clear EXTI pending so a re-trigger can set the event again
     * (e.g. after an aborted glitch). */
    EXTI->PR = EXTI_PR_PR16;
}

/* ------------------------------------------------------------------
 * Task-context handler. Called from StartMyTask every ~1ms.
 * Fast return when no event is pending. When an event is set, runs the
 * soft-delay (~3-5ms) PVDO verification window:
 *   3 checks, ~2ms apart (vTaskDelay yields CPU, no DWT busy-wait).
 *   Retry rounds are bound to g_pvd_trigger_count: the round number is
 *   generated from the start counter (base_count), so the log reflects
 *   the actual trigger count instead of a hard-coded 1/2/3.
 *   All 3 must confirm PVDO=1 -> power failure.
 *   Any PVDO=0 -> voltage recovered, abort.
 * After the round, g_pvd_event_flag and g_pvd_trigger_count are cleared.
 * ------------------------------------------------------------------ */
void pvd_poll_handler(void)
{
    if (!system_ready) {
        return;
    }

    /* No event pending - fast return (called every ~1ms from myTask) */
    if (!g_pvd_event_flag) {
        return;
    }

    /* Drain UART / DMA so our diagnostic lines come out cleanly */
    pvd_wait_uart_idle();

    pvd_direct_log("PVD ISR: entered\r\n");

    static uint8_t base_pvd_trigger_count = 0;

	if (base_pvd_trigger_count++ < PVD_ISR_RETRY_MAX_COUNT) {

		uint8_t pvd_flag = __HAL_PWR_GET_FLAG(PWR_FLAG_PVDO) ? 1 : 0;

		/* Round number bound to trigger count: base_count + i */
		char retry_buf[24];
		int ret = snprintf(retry_buf, sizeof(retry_buf),
				"[PVD] ISR: retry %u\r\n", (unsigned int) (base_pvd_trigger_count));
		if (ret > 0) {
			pvd_direct_log(retry_buf);
		}

		if (!pvd_flag) {
			/* Voltage recovered before window completes */
	        pvd_direct_log("[PVD] voltage recovered, abort\r\n");
		    /* Clear trigger counter for this monitoring round */
		    g_pvd_event_flag = 0;
		    base_pvd_trigger_count = 0;
		}
	} else {

	    /* Clear trigger counter for this monitoring round */
	    g_pvd_event_flag = 0;
	    base_pvd_trigger_count = 0;

		/* --- PVDO confirmed low for the full window: power failure --- */
		g_power_failure = 1;

		pvd_direct_log("[PVD]power failure confirmed\r\n");
#if (!ELOG_FILE_SYNC_ON_WRITE)
		/* Flush all pending log data to SPI Flash */
		elog_file_flush_all_isr();
		pvd_direct_log("[PVD]flushing logs\r\n");

		spi_wait_for_idle();
		flash_wait_internal();
#endif
		pvd_direct_log("[PVD]system reset\r\n");

		/* Ensure diagnostic bytes have left the UART shift register */
		while (!(USART1->SR & USART_SR_TC)) {
		}

		NVIC_SystemReset();
	}
}
