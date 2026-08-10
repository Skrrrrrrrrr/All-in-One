/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * File Name          : freertos.c
  * Description        : Code for freertos applications
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */

/* Includes ------------------------------------------------------------------*/
#include "FreeRTOS.h"
#include "task.h"
#include "main.h"
#include "cmsis_os.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "usart.h"
#include "dma.h"
#include "spi.h"
#include "bsp_init.h"
#include "elog.h"
#include "elog_file.h"
#include "pvd_detection.h"
#include "uart_ringbuf.h"
#include "serial.h"
#include "ota_core.h"

/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
/* USER CODE BEGIN Variables */
/* Definitions for myTask */
osThreadId_t myTaskHandle;
const osThreadAttr_t myTask_attributes = {
  .name = "myTask",
  .stack_size = 3072 * 4,
  .priority = (osPriority_t) osPriorityLow,
};

extern uint8_t usart_rx_over;
extern uint8_t rx_buff[USART_BUFF_SIZE];
extern DMA_HandleTypeDef hdma_usart1_rx;
extern DMA_HandleTypeDef hdma_usart1_tx;

/* USER CODE END Variables */
/* Definitions for defaultTask */
osThreadId_t defaultTaskHandle;
const osThreadAttr_t defaultTask_attributes = {
  .name = "defaultTask",
  .stack_size = 128 * 4,
  .priority = (osPriority_t) osPriorityNormal,
};

/* Private function prototypes -----------------------------------------------*/
/* USER CODE BEGIN FunctionPrototypes */

extern volatile BaseType_t xInUserInputMode;
extern volatile uint8_t ucInputIndex;
void vSetPromptRefreshNeeded( void );

#if defined(__GNUC__)
int _write(int fd, char *ptr, int len) {
    if (fd == 1 || fd == 2) {
        BaseType_t xInInputMode = xInUserInputMode;

        if (xInInputMode == pdTRUE) {
            uart_rb_write((uint8_t*)"\r", 1);
            uint8_t i;
            for (i = 0; i < ucInputIndex + 2; i++) {
                uart_rb_write((uint8_t*)" ", 1);
            }
            uart_rb_write((uint8_t*)"\r", 1);
        }

        uart_rb_write((uint8_t*)ptr, len);

        if (xInInputMode == pdTRUE) {
            vSetPromptRefreshNeeded();
        }

        return len;
    }
    return -1;
}
#endif

static void system_pre_init(void) {
    HAL_UARTEx_ReceiveToIdle_DMA(&huart1, rx_buff, USART_BUFF_SIZE);
    __HAL_DMA_DISABLE_IT(&hdma_usart1_rx, DMA_IT_HT);

    uint32_t reset_flags = RCC->CSR;
    char reset_reason[128] = "Reset: ";
    if (reset_flags & RCC_CSR_PORRSTF) strcat(reset_reason, "POR/PDR ");
    if (reset_flags & RCC_CSR_BORRSTF) strcat(reset_reason, "BOR ");
    if (reset_flags & RCC_CSR_PINRSTF) strcat(reset_reason, "Pin ");
    if (reset_flags & RCC_CSR_SFTRSTF) strcat(reset_reason, "Software ");
    if (reset_flags & RCC_CSR_IWDGRSTF) strcat(reset_reason, "IWDG ");
    if (reset_flags & RCC_CSR_WWDGRSTF) strcat(reset_reason, "WWDG ");
    if (reset_flags & RCC_CSR_LPWRRSTF) strcat(reset_reason, "LowPower ");
    if (reset_flags == 0) strcpy(reset_reason, "Reset: Unknown");
    RCC->CSR |= RCC_CSR_RMVF;

    char reset_msg[160];
    sprintf(reset_msg, "%s(CSR=0x%08X)\r\n", reset_reason, (unsigned int)reset_flags);
    uart_rb_write((uint8_t*)reset_msg, strlen(reset_msg));
}
void StartMyTask(void *argument);
/* USER CODE END FunctionPrototypes */

void StartDefaultTask(void *argument);

extern void MX_LWIP_Init(void);
void MX_FREERTOS_Init(void); /* (MISRA C 2004 rule 8.1) */

/**
  * @brief  FreeRTOS initialization
  * @param  None
  * @retval None
  */
void MX_FREERTOS_Init(void) {
  /* USER CODE BEGIN Init */
	system_pre_init();
  /* USER CODE END Init */

  /* USER CODE BEGIN RTOS_MUTEX */
  /* add mutexes, ... */
  /* USER CODE END RTOS_MUTEX */

  /* USER CODE BEGIN RTOS_SEMAPHORES */
  /* add semaphores, ... */
  /* USER CODE END RTOS_SEMAPHORES */

  /* USER CODE BEGIN RTOS_TIMERS */
  /* start timers, add new ones, ... */
  /* USER CODE END RTOS_TIMERS */

  /* USER CODE BEGIN RTOS_QUEUES */
  /* add queues, ... */
  /* USER CODE END RTOS_QUEUES */

  /* Create the thread(s) */
  /* creation of defaultTask */
  defaultTaskHandle = osThreadNew(StartDefaultTask, NULL, &defaultTask_attributes);

  /* USER CODE BEGIN RTOS_THREADS */
  /* add threads, ... */
  myTaskHandle = osThreadNew(StartMyTask, NULL, &myTask_attributes);
  /* USER CODE END RTOS_THREADS */

  /* USER CODE BEGIN RTOS_EVENTS */
  /* add events, ... */
  /* USER CODE END RTOS_EVENTS */

}

/* USER CODE BEGIN Header_StartDefaultTask */
/**
  * @brief  Function implementing the defaultTask thread.
  * @param  argument: Not used
  * @retval None
  */
/* USER CODE END Header_StartDefaultTask */
void StartDefaultTask(void *argument)
{
  /* init code for LWIP */
  MX_LWIP_Init();
  /* USER CODE BEGIN StartDefaultTask */
  /* Infinite loop */
  for(;;)
  {
    osDelay(1000);
    HAL_GPIO_TogglePin(LED0_GPIO_Port, LED0_Pin);
  }
  /* USER CODE END StartDefaultTask */
}

/* Private application code --------------------------------------------------*/
/* USER CODE BEGIN Application */
void StartMyTask(void *argument)
{
	int bsp_ret = bsp_init(&huart1, &hspi1);
	if (bsp_ret != 0) {
		uart_rb_write((uint8_t*) "BSP init failed!\r\n", sizeof("BSP init failed!\r\n"));
	}
    pvd_init();
    elog_set_filter_lvl(ELOG_LVL_ERROR);
    pvd_mark_ready();

    vRegisterSampleCLICommands();
    /* CLI task priority lowered below myTask (osPriorityLow): it only
     * polls the UART, so a PVD event handled by myTask must never wait
     * for the CLI to yield. */
    vUARTCommandConsoleStart(1024, osPriorityIdle);

    /* OTA：初始化参数区并启动vOTATask（静态分配，常驻后台接收升级命令�??????? */
    ota_init();
    ota_task_start();

//    MX_LWIP_Init();
    /* Infinite loop */
    for (;;) {

        /* Soft-delay poll period ~1ms: yield CPU, then check PVD event.
         * pvd_poll_handler fast-returns when no event is pending. */
        osDelay(1);
		pvd_poll_handler();

#if (!ELOG_FILE_SYNC_ON_WRITE)
        static uint32_t last_flush_tick = 0;
        uint32_t now = HAL_GetTick();
        if (now - last_flush_tick >= ELOG_FILE_FLUSH_INTERVAL_MS) {
            elog_file_flush_all();
            last_flush_tick = now;
        }
#endif

    }
}

void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName)
{
    (void)xTask;
    uart_rb_write((uint8_t*) "\r\n[FATAL] Stack overflow in: ", sizeof("\r\n[FATAL] Stack overflow in: ") - 1);
    uart_rb_write((uint8_t*)pcTaskName, strlen(pcTaskName));
    uart_rb_write((uint8_t*) "\r\n", 2);
    taskDISABLE_INTERRUPTS();
    for(;;);
}

void vApplicationMallocFailedHook(void)
{
    uart_rb_write((uint8_t*) "\r\n[FATAL] malloc failed!\r\n", sizeof("\r\n[FATAL] malloc failed!\r\n") - 1);
    taskDISABLE_INTERRUPTS();
    for(;;);
}

/* USER CODE END Application */

