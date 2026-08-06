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
#include "uart_ringbuf.h"

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
void elog_minitor_test(void);

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
    vUARTCommandConsoleStart(1024, osPriorityNormal);

    /* Infinite loop */
    for (;;) {

        osDelay(2000);
        log_d("Debug message from myTask");
//        uart_rb_write((uint8_t*) "Mytask running!\r\n", sizeof("Mytask running!\r\n"));
#if (!ELOG_FILE_SYNC_ON_WRITE)
        static int flush_counter = 0;
        flush_counter++;
        if (flush_counter >= (ELOG_FILE_FLUSH_INTERVAL_MS / 1000)) {
            elog_file_flush_all();
            flush_counter = 0;
        }
#endif

    }
}
void elog_minitor_test(void) {
    static int monitor_counter = 0;
    monitor_counter++;
    if (monitor_counter >= 2) {
        monitor_counter = 0;

        log_i("=== System Monitor ===");

        log_i("--- Running Tasks ---");
        char taskListBuf[512];
        vTaskList(taskListBuf);
        log_i("Name          State  Priority  Stack  Num");
        log_i("%s", taskListBuf);

        log_i("--- Log Files ---");
        elog_file_port_lock();
        lfs_t *pLfs = elog_file_port_get_lfs();
        struct lfs_config *pLfsCfg = elog_file_port_get_lfs_config();
        if (pLfs != NULL && pLfsCfg != NULL) {
            lfs_dir_t dir;
            int err = lfs_dir_open(pLfs, &dir, "/");
            if (err == LFS_ERR_OK) {
                struct lfs_info info;
                int file_count = 0;
                while (lfs_dir_read(pLfs, &dir, &info) > 0) {
                    if ((strcmp(info.name, ".") == 0)
                            || (strcmp(info.name, "..") == 0)) {
                        continue;
                    }
                    if (info.type == LFS_TYPE_REG) {
                        log_i("File: %s, Size: %ld bytes", info.name,
                                info.size);
                        file_count++;
                    } else if (info.type == LFS_TYPE_DIR) {
                        log_i("Dir:  %s", info.name);
                    }
                }
                lfs_dir_close(pLfs, &dir);
                if (file_count == 0) {
                    log_i("No files found");
                }
            } else {
                log_e("Failed to open directory, err=%d", err);
            }

            log_i("--- Flash Usage ---");
            lfs_ssize_t used_blocks = lfs_fs_size(pLfs);
            if (used_blocks >= 0) {
                uint32_t used_bytes = (uint32_t) used_blocks
                        * pLfsCfg->block_size;
                uint32_t total_bytes = pLfsCfg->block_count
                        * pLfsCfg->block_size;
                uint32_t free_bytes = total_bytes - used_bytes;
                float used_percent = ((float) used_bytes / total_bytes) * 100;
                log_i("Used: %lu bytes (%lu blocks), Free: %lu bytes",
                        used_bytes, (uint32_t )used_blocks, free_bytes);
                log_i("Total: %lu bytes, Usage: %.1f%%", total_bytes,
                        used_percent);
            } else {
                log_e("Failed to get flash usage, err=%ld", (long )used_blocks);

                if (used_blocks == LFS_ERR_CORRUPT) {
                    log_w("Filesystem is corrupt, reformatting...");

                    elog_file_port_unlock();
                    elog_file_deinit();
                    elog_file_port_lock();

                    int fmt_err = lfs_format(pLfs, pLfsCfg);
                    if (fmt_err == LFS_ERR_OK) {
                        fmt_err = lfs_mount(pLfs, pLfsCfg);
                        if (fmt_err == LFS_ERR_OK) {
                            log_i("Filesystem reformatted successfully");
                            used_blocks = lfs_fs_size(pLfs);
                            if (used_blocks >= 0) {
                                log_i("New used: %lu blocks",
                                        (uint32_t )used_blocks);
                            }

                            elog_file_port_unlock();
                            elog_file_init();
                            elog_file_port_lock();
                        } else {
                            log_e("Re-mount failed after format, err=%d",
                                    fmt_err);
                        }
                    } else {
                        log_e("Format failed, err=%d", fmt_err);
                    }
                }
            }
        } else {
            log_e("LFS not initialized");
        }
        elog_file_port_unlock();

        log_i("=== End Monitor ===");
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

