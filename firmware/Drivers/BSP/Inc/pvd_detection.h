/*
 * pvd_detection.h
 *
 * Power Voltage Detector (PVD) for power failure detection.
 * Triggers when VDD drops below the configured threshold (2.7V).
 * On detection, flush all pending logs to SPI Flash before power dies.
 *
 * Hardware: STM32F407 internal PVD + EXTI Line 16
 * External: VDD bypass capacitor (>= 100uF) to hold power for ~5-10ms
 */

#ifndef __PVD_DETECTION_H__
#define __PVD_DETECTION_H__

#include "main.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PVD_THRESHOLD_LEVEL         PWR_PVDLEVEL_5   /* 2.7V */

extern volatile uint8_t g_power_failure;

extern volatile uint8_t g_pvd_trigger_count;
extern volatile uint32_t g_pvd_trigger_timestamp;

void pvd_init(void);
void pvd_mark_ready(void);

/* Event-signal flow (soft-delay monitoring):
 *   ISR             -> set g_pvd_event_flag (internal)
 *   myTask          -> pvd_poll_handler() every ~1ms, fast return if idle.
 * Handler runs the soft-delay (~3-5ms) PVDO verification + shutdown.
 * After each round, g_pvd_event_flag and g_pvd_trigger_count are cleared. */
void pvd_poll_handler(void);

#ifdef __cplusplus
}
#endif

#endif /* __PVD_DETECTION_H__ */
