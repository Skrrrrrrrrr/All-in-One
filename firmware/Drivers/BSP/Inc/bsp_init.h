/*
 * bsp_init.h
 * BSP初始化中心模块，负责各驱动模块的注册和初始化
 */

#ifndef _BSP_INIT_H_
#define _BSP_INIT_H_

#include "stm32f4xx_hal.h"
#include "cmsis_os.h"

int bsp_init(UART_HandleTypeDef *huart, SPI_HandleTypeDef *hspi);

#endif /* _BSP_INIT_H_ */
