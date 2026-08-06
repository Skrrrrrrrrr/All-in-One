/*
 * pvd_detection.h
 *
 * Power Voltage Detector (PVD) for power failure detection.
 * Triggers when VDD drops below the configured threshold (2.7V).
 * On detection, flush all pending logs to SPI Flash before power dies.
 *
 * Hardware: STM32F411 internal PVD + EXTI Line 16
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
void pvd_simulate_trigger(void);
void pvd_simulate_trigger_with_power_fail(void);

#ifdef __cplusplus
}
#endif

#endif /* __PVD_DETECTION_H__ */