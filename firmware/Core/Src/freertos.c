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
#include "FreeRTOS_CLI.h"
#include "uart_ringbuf.h"
#include "serial.h"
#include "lwip_test_cmds.h"
#include <string.h>          /* strlen()：栈溢出钩子中打印任务名 */
#include "bsp_init.h"        /* bsp_init()：BSP 各驱动模块注册初始化 */
#include "elog.h"            /* elog_set_filter_lvl()：EasyLogger 过滤级别 */
#include "pvd_detection.h"   /* pvd_init/pvd_mark_ready/pvd_poll_handler() */
#include "ota_core.h"        /* ota_init/ota_task_start()：A/B 双区 OTA */
/* 注：elog_file.h 未被本文件直接使用（日志落盘�? pvd_detection.c 内）�?
 * uart_ringbuf.h / serial.h 已在上方包含，故不再重复放开 */

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
  /* �????? 4096 字节(1K words)�?????
   * myTask 在本 LwIP 测试版中内联执行 MX_LWIP_Init()，调用链
   *  StartMyTask→MX_LWIP_Init→netif_add→ethernetif_init→low_level_init
   *  →HAL_ETH_Init/DP83848_Init�?????-O0 下峰值约 1.5KB；且�????? USART1/ETH
   *  中断抢占�????? ISR 帧复用本任务栈�?�故�????? 4096 而非�????? OTA 版的 2048�?????
   * �????? 3072*4=12KB �????? OTA/PVD 版残留，占用堆过多直接挤�????? 28KB 堆�?? */
  .stack_size = 4096,
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
/* 复位原因诊断输出（USART1 寄存器轮询）�?????
 * system_pre_init �????? MX_FREERTOS_Init（创建任务�?�启动调度器）之前调用，
 * 此时 UART DMA / 环形缓冲 / 队列均不可用，故直接轮询 USART1 寄存�?????
 * 输出 RCC->CSR 复位标志。用于区分复位来源（POR/PIN/软件/看门�?????/掉电），
 * 帮助定位"死机-复位"循环（如断言挂死后被人为复位、或 BOR 掉电复位）�?? */
static void uart_poll_puts(const char *s)
{
    if (s == NULL) {
        return;
    }
    while (*s != '\0') {
        while ((USART1->SR & USART_SR_TXE) == 0U) {}
        USART1->DR = (uint8_t)*s;
        s++;
    }
}

static void uart_poll_puthex(uint32_t v)
{
    static const char hex_digits[] = "0123456789ABCDEF";
    char tmp[9];
    int i;

    for (i = 0; i < 8; i++) {
        tmp[i] = hex_digits[(v >> ((7 - i) * 4)) & 0x0Fu];
    }
    tmp[8] = '\0';
    uart_poll_puts(tmp);
}

static void system_pre_init(void) {
    uint32_t reset_flags = RCC->CSR;

    /* 清零 CCMRAM BSS 段（.ccmram_bss 为 NOLOAD，startup 仅清普通 .bss）。
     * OTA/日志/CLI 缓冲在调度器启动后才被使用，此处清零必须先于一切使用点。 */
    extern uint8_t _sccmram_bss;
    extern uint8_t _eccmram_bss;
    {
        uint8_t *p = &_sccmram_bss;
        uint32_t len = (uint32_t)&_eccmram_bss - (uint32_t)&_sccmram_bss;

        while (len > 0u) {
            *p = 0u;
            p++;
            len--;
        }
    }

    uart_poll_puts("\r\n[BOOT] RCC_CSR=0x");
    uart_poll_puthex(reset_flags);
    uart_poll_puts(" (");

    if (reset_flags & RCC_CSR_LPWRRSTF) uart_poll_puts("LOWPOWER ");
    if (reset_flags & RCC_CSR_WWDGRSTF) uart_poll_puts("WWDG ");
    if (reset_flags & RCC_CSR_IWDGRSTF) uart_poll_puts("IWDG ");
    if (reset_flags & RCC_CSR_SFTRSTF)  uart_poll_puts("SOFTWARE ");
    if (reset_flags & RCC_CSR_PORRSTF)  uart_poll_puts("POR ");
    if (reset_flags & RCC_CSR_PINRSTF)  uart_poll_puts("PIN ");
    if (reset_flags & RCC_CSR_BORRSTF)  uart_poll_puts("BOR ");
    if (reset_flags == 0)               uart_poll_puts("NONE");

    uart_poll_puts(")\r\n");
    /* 清除复位标志：保证下�?????次复位时读到的是当次真实的复位来�????? */
    RCC->CSR |= RCC_CSR_RMVF;
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
//  MX_LWIP_Init();
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
    /* 注册 LwIP 网络诊断命令（ifconfig/arp/route/ping/tcp_test/
     * udp_test/lwip_test），�? CLI 任务启动前注册，命令执行�? LwIP 已初始化 */
    vRegisterLwipTestCommands();
    /* CLI task priority lowered below myTask (osPriorityLow): it only
     * polls the UART, so a PVD event handled by myTask must never wait
     * for the CLI to yield. */
    vUARTCommandConsoleStart(1024, osPriorityIdle);

    /* CLI 任务完成 xSerialPortInitMinimal() 创建接收队列后再启动
     * UART RX DMA：避免中断回调向未创建的空队列投递字符触发断�?�?
     * 之后每次 IDLE 完成�? usart.c �? HAL_UARTEx_RxEventCallback 重新
     * 启动接收（RX 链路的唯�?首次启动点，缺失�? CLI 收不到任何输入）�? */
    osDelay(200);
    HAL_UARTEx_ReceiveToIdle_DMA(&huart1, rx_buff, USART_BUFF_SIZE);
    __HAL_DMA_DISABLE_IT(&hdma_usart1_rx, DMA_IT_HT);

    /* OTA：初始化参数区并启动vOTATask（静态分配，常驻后台接收升级命令�???????????????? */
    ota_init();
    ota_task_start();

    /* init code for LWIP */
    /* 手动 LwIP �????后初始化（defaultTask 中的 MX_LWIP_Init 已注释）�????
     * 本任务先完成 uart_rb_register/CLI 启动，再调用 MX_LWIP_Init()�????
     * 保证 LwIP 初始化时串口日志通道已就绪�??
     * lwip.c 内另�????"幂等 + uart_rb 就绪"守卫，即�???? defaultTask 再次
     * 调用也不会重复初始化（曾因重�???? tcpip_init/netif_add 导致
     * netif 链表自环、信号量/线程重复创建，触�???? port.c:415 断言）�?? */
    MX_LWIP_Init();

    /* Infinite loop */
    for (;;) {

        /* Soft-delay poll period ~1ms: yield CPU, then check PVD event.
         * pvd_poll_handler fast-returns when no event is pending. */
        osDelay(1);
        pvd_poll_handler();

//#if (!ELOG_FILE_SYNC_ON_WRITE)
//        static uint32_t last_flush_tick = 0;
//        uint32_t now = HAL_GetTick();
//        if (now - last_flush_tick >= ELOG_FILE_FLUSH_INTERVAL_MS) {
//            elog_file_flush_all();
//            last_flush_tick = now;
//        }
//#endif

    }
}

/* 栈溢出钩子：FreeRTOS �?????测到任务栈溢出（configCHECK_FOR_STACK_OVERFLOW=2�?????
 * 时调用�?�输出肇事任务名，然后挂起�?��?�用于快速区�?????"栈溢�?????"死机�?????
 * 注意：此钩子在任务上下文执行，uart_rb_write 安全�????? */
void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName)
{
    (void)xTask;
    uart_rb_write((uint8_t*) "\r\n[FATAL] Stack overflow in: ", sizeof("\r\n[FATAL] Stack overflow in: ") - 1);
    uart_rb_write((uint8_t*)pcTaskName, strlen(pcTaskName));
    uart_rb_write((uint8_t*) "\r\n", 2);
    taskDISABLE_INTERRUPTS();
    for(;;);
}

/* 内存分配失败钩子：pvPortMalloc 失败（堆耗尽）时调用�?????次，
 * 用于区分"内存不足"类死机�?? */
void vApplicationMallocFailedHook(void)
{
    uart_rb_write((uint8_t*) "\r\n[FATAL] malloc failed!\r\n", sizeof("\r\n[FATAL] malloc failed!\r\n") - 1);
    taskDISABLE_INTERRUPTS();
    for(;;);
}



/* 十进制输出（寄存器轮询，任何上下文安全）；用于打�???? IPSR/VECTACTIVE */
static void uart_poll_putdec(uint32_t v)
{
    char digits[12];
    int idx = 0;

    do {
        digits[idx++] = (char)('0' + (v % 10));
        v /= 10;
    } while (v > 0);

    while (idx > 0) {
        idx--;
        while ((USART1->SR & USART_SR_TXE) == 0U) {}
        USART1->DR = (uint8_t)digits[idx];
    }
}

/* FreeRTOS 断言增强诊断：在原有 "文件:行号" 基础上追加上下文信息，用�????
 * 精确定位 port.c:415 这类"ISR 中调用非 FromISR 临界�????"断言�????
 *   [A] IPSR=xx VECTACTIVE=xx
 *       当前异常编号�????0=线程模式）�?�port.c:415 断言等价�???? VECTACTIVE!=0�????
 *       此�?�可直接映射�???? NVIC 中断号（VECTACTIVE - 16 = IRQn），
 *       从�?�锁定是哪个外设中断调用�???? taskENTER_CRITICAL�????
 *   [A] frame PC=0x... LR=0x...
 *       �???? Handler 栈（MSP）向上扫描，�????"xPSR �???? Thumb �???? + PC 落在 Flash"
 *       特征定位硬件异常帧，打印被中断现场的 PC/LR�????
 * 全部使用寄存器轮询输出：断言本身常由临界�????/中断违例引起，此时调度器�????
 * DMA、环形缓冲均不可信，�???? PVD/复位诊断同一策略�???? */
void prvAssertFailPrint(const char *file, int line)
{
    const char *p;
    const char *base;
    int n = line;
    uint32_t ipsr;
    uint32_t vectactive;

    /* 输出固定前缀 */
    p = "\r\n[ASSERT] ";
    while (*p != '\0') {
        while ((USART1->SR & USART_SR_TXE) == 0U) {}
        USART1->DR = (uint8_t)*p;
        p++;
    }

    /* 仅打印文件名（截取最后一�????? '/' �????? '\\' 之后的部分） */
    base = file;
    if (file != NULL) {
        while (*file != '\0') {
            if ((*file == '/') || (*file == '\\')) {
                base = file + 1;
            }
            file++;
        }
        p = base;
        while (*p != '\0') {
            while ((USART1->SR & USART_SR_TXE) == 0U) {}
            USART1->DR = (uint8_t)*p;
            p++;
        }
    }

    p = ":";
    while (*p != '\0') {
        while ((USART1->SR & USART_SR_TXE) == 0U) {}
        USART1->DR = (uint8_t)*p;
        p++;
    }

    /* 行号转十进制后�?�位输出 */
    if (n < 0) {
        n = -n;
    }
    {
        char digits[12];
        int idx = 0;

        do {
            digits[idx++] = (char)('0' + (n % 10));
            n /= 10;
        } while (n > 0);
        while (idx > 0) {
            idx--;
            while ((USART1->SR & USART_SR_TXE) == 0U) {}
            USART1->DR = (uint8_t)digits[idx];
        }
    }

    /* ---- 增强诊断：断�????上下文（当前异常�???? + 被中�???? PC/LR�???? ---- */
    ipsr = __get_IPSR();                    /* 当前执行异常编号 */
    vectactive = (SCB->ICSR) & 0x1FFUL;     /* ICSR[8:0]：活动异常号 */

    uart_poll_puts("\r\n[A] IPSR=");
    uart_poll_putdec(ipsr);
    uart_poll_puts(" VECTACTIVE=");
    uart_poll_putdec(vectactive);

    /* 尝试�???? MSP 向上扫描，按异常帧特征定位被中断现场 */
    {
        volatile uint32_t *sp = (volatile uint32_t *)__get_MSP();
        volatile uint32_t *end = sp + 64;
        uint32_t found = 0;

        while (sp < end) {
            uint32_t xpsr = sp[7];
            uint32_t pc   = sp[6];

            /* 异常�???? xPSR �???? Thumb �????(bit24)必须置位，且 PC 落在 Flash 程序�???? */
            if (((xpsr & 0x01000000UL) != 0UL) &&
                ((pc & 0xFFF00000UL) == 0x08000000UL)) {
                uart_poll_puts("\r\n[A] frame PC=0x");
                uart_poll_puthex(pc);
                uart_poll_puts(" LR=0x");
                uart_poll_puthex(sp[5]);
                found = 1;
                break;
            }
            sp++;
        }
        if (found == 0) {
            uart_poll_puts("\r\n[A] frame not-found");
        }
    }

    p = "\r\n";
    while (*p != '\0') {
        while ((USART1->SR & USART_SR_TXE) == 0U) {}
        USART1->DR = (uint8_t)*p;
        p++;
    }
}

/* USER CODE END Application */

