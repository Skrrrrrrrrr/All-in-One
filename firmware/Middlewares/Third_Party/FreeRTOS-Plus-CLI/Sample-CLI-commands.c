/*
 * FreeRTOS V202212.00
 * Copyright (C) 2020 Amazon.com, Inc. or its affiliates. All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of
 * this software and associated documentation files (the "Software"), to deal in
 * the Software without restriction, including without limitation the rights to
 * use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
 * the Software, and to permit persons to whom the Software is furnished to do so,
 * subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
 * FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
 * COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
 * IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 * https://www.FreeRTOS.org
 * https://github.com/FreeRTOS
 *
 */

/******************************************************************************
*
* https://www.FreeRTOS.org/cli
*
******************************************************************************/


/* FreeRTOS includes. */
#include "FreeRTOS.h"
#include "task.h"
#include "cmsis_os2.h"

/* Standard includes. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* FreeRTOS+CLI includes. */
#include "FreeRTOS_CLI.h"

/* Project includes. */
#include "stm32f4xx_hal.h"
#include "lfs.h"
#include "elog.h"
#include "elog_file.h"
#include "uart_ringbuf.h"
#include "pvd_detection.h"
#include "sfud.h"

/* Auto-detect CPU model from compile-time macro. */
#if defined(STM32F407xx)
    #define CPU_MODEL_STRING "STM32F407"
#elif defined(STM32F411xE)
    #define CPU_MODEL_STRING "STM32F411"
#elif defined(STM32F401xE)
    #define CPU_MODEL_STRING "STM32F401"
#elif defined(STM32F401xC)
    #define CPU_MODEL_STRING "STM32F401"
#elif defined(STM32F429xx)
    #define CPU_MODEL_STRING "STM32F429"
#elif defined(STM32F446xx)
    #define CPU_MODEL_STRING "STM32F446"
#elif defined(STM32F469xx)
    #define CPU_MODEL_STRING "STM32F469"
#else
    #define CPU_MODEL_STRING "STM32F4xx"
#endif

#ifndef  configINCLUDE_TRACE_RELATED_CLI_COMMANDS
    #define configINCLUDE_TRACE_RELATED_CLI_COMMANDS    0
#endif

#ifndef configINCLUDE_QUERY_HEAP_COMMAND
    #define configINCLUDE_QUERY_HEAP_COMMAND    0
#endif

/*
 * The function that registers the commands that are defined within this file.
 */
void vRegisterSampleCLICommands( void );

/*
 * Implements the task-stats command.
 */
static BaseType_t prvTaskStatsCommand( char * pcWriteBuffer,
                                       size_t xWriteBufferLen,
                                       const char * pcCommandString );

/*
 * Implements the run-time-stats command.
 */
#if ( configGENERATE_RUN_TIME_STATS == 1 )
    static BaseType_t prvRunTimeStatsCommand( char * pcWriteBuffer,
                                              size_t xWriteBufferLen,
                                              const char * pcCommandString );
#endif /* configGENERATE_RUN_TIME_STATS */

/*
 * Implements the echo-three-parameters command.
 */
static BaseType_t prvThreeParameterEchoCommand( char * pcWriteBuffer,
                                                size_t xWriteBufferLen,
                                                const char * pcCommandString );

/*
 * Implements the echo-parameters command.
 */
static BaseType_t prvParameterEchoCommand( char * pcWriteBuffer,
                                           size_t xWriteBufferLen,
                                           const char * pcCommandString );

/*
 * Implements the "query heap" command.
 */
#if ( configINCLUDE_QUERY_HEAP_COMMAND == 1 )
    static BaseType_t prvQueryHeapCommand( char * pcWriteBuffer,
                                           size_t xWriteBufferLen,
                                           const char * pcCommandString );
#endif

/*
 * Implements the "trace start" and "trace stop" commands;
 */
#if ( configINCLUDE_TRACE_RELATED_CLI_COMMANDS == 1 )
    static BaseType_t prvStartStopTraceCommand( char * pcWriteBuffer,
                                                size_t xWriteBufferLen,
                                                const char * pcCommandString );
#endif

/* Project specific commands. */
static BaseType_t prvTasksCommand(char *pcWriteBuffer, size_t xWriteBufferLen, const char *pcCommandString);
static BaseType_t prvHeapCommand(char *pcWriteBuffer, size_t xWriteBufferLen, const char *pcCommandString);
static BaseType_t prvFlashCommand(char *pcWriteBuffer, size_t xWriteBufferLen, const char *pcCommandString);
static BaseType_t prvLogCommand(char *pcWriteBuffer, size_t xWriteBufferLen, const char *pcCommandString);
static BaseType_t prvResetCommand(char *pcWriteBuffer, size_t xWriteBufferLen, const char *pcCommandString);
static BaseType_t prvStatsCommand(char *pcWriteBuffer, size_t xWriteBufferLen, const char *pcCommandString);
static BaseType_t prvPvdCommand(char *pcWriteBuffer, size_t xWriteBufferLen, const char *pcCommandString);
static BaseType_t prvShellCommand(char *pcWriteBuffer, size_t xWriteBufferLen, const char *pcCommandString);

/* Structure that defines the "task-stats" command line command.  This generates
 * a table that gives information on each task in the system. */
static const CLI_Command_Definition_t xTaskStats =
{
    "task-stats",        /* The command string to type. */
    "\r\ntask-stats:\r\n Displays a table showing the state of each FreeRTOS task\r\n",
    prvTaskStatsCommand, /* The function to run. */
    0                    /* No parameters are expected. */
};

/* Structure that defines the "echo_3_parameters" command line command.  This
 * takes exactly three parameters that the command simply echos back one at a
 * time. */
static const CLI_Command_Definition_t xThreeParameterEcho =
{
    "echo-3-parameters",
    "\r\necho-3-parameters <param1> <param2> <param3>:\r\n Expects three parameters, echos each in turn\r\n",
    prvThreeParameterEchoCommand, /* The function to run. */
    3                             /* Three parameters are expected, which can take any value. */
};

/* Structure that defines the "echo_parameters" command line command.  This
 * takes a variable number of parameters that the command simply echos back one at
 * a time. */
static const CLI_Command_Definition_t xParameterEcho =
{
    "echo-parameters",
    "\r\necho-parameters <...>:\r\n Take variable number of parameters, echos each in turn\r\n",
    prvParameterEchoCommand, /* The function to run. */
    -1                       /* The user can enter any number of commands. */
};

#if ( configGENERATE_RUN_TIME_STATS == 1 )

/* Structure that defines the "run-time-stats" command line command.   This
 * generates a table that shows how much run time each task has */
    static const CLI_Command_Definition_t xRunTimeStats =
    {
        "run-time-stats",       /* The command string to type. */
        "\r\nrun-time-stats:\r\n Displays a table showing how much processing time each FreeRTOS task has used\r\n",
        prvRunTimeStatsCommand, /* The function to run. */
        0                       /* No parameters are expected. */
    };
#endif /* configGENERATE_RUN_TIME_STATS */

#if ( configINCLUDE_QUERY_HEAP_COMMAND == 1 )
    /* Structure that defines the "query_heap" command line command. */
    static const CLI_Command_Definition_t xQueryHeap =
    {
        "query-heap",
        "\r\nquery-heap:\r\n Displays the free heap space, and minimum ever free heap space.\r\n",
        prvQueryHeapCommand, /* The function to run. */
        0                    /* The user can enter any number of commands. */
    };
#endif /* configQUERY_HEAP_COMMAND */

#if configINCLUDE_TRACE_RELATED_CLI_COMMANDS == 1

/* Structure that defines the "trace" command line command.  This takes a single
 * parameter, which can be either "start" or "stop". */
    static const CLI_Command_Definition_t xStartStopTrace =
    {
        "trace",
        "\r\ntrace [start | stop]:\r\n Starts or stops a trace recording for viewing in FreeRTOS+Trace\r\n",
        prvStartStopTraceCommand, /* The function to run. */
        1                         /* One parameter is expected.  Valid values are "start" and "stop". */
    };
#endif /* configINCLUDE_TRACE_RELATED_CLI_COMMANDS */

/* Project specific command definitions. */
static const CLI_Command_Definition_t xTasksCommand = {
    "tasks",
    "\r\ntasks:\r\n Displays a table showing the state of each FreeRTOS task\r\n",
    prvTasksCommand,
    0
};

static const CLI_Command_Definition_t xHeapCommand = {
    "heap",
    "\r\nheap:\r\n Displays current and minimum ever free heap size\r\n",
    prvHeapCommand,
    0
};

static const CLI_Command_Definition_t xFlashCommand = {
    "flash",
    "\r\nflash:\r\n Displays files in Flash and filesystem usage\r\n",
    prvFlashCommand,
    0
};

static const CLI_Command_Definition_t xLogCommand = {
    "log",
    "\r\nlog [level <lvl> | tail [<count> [<filename>]] | tail all [<filename>]]:\r\n level - set log level (v/d/i/w/e)\r\n tail - display last 10 log lines\r\n tail <count> - display last <count> log lines\r\n tail <count> <file> - display last <count> lines from <file>\r\n tail all - display all log lines\r\n tail all <file> - display all lines from <file>\r\n",
    prvLogCommand,
    -1
};

static const CLI_Command_Definition_t xResetCommand = {
    "reset",
    "\r\nreset:\r\n Performs a software reset of the system\r\n",
    prvResetCommand,
    0
};

static const CLI_Command_Definition_t xStatsCommand = {
    "stats",
    "\r\nstats:\r\n Displays system statistics (UART ring buffer, etc.)\r\n",
    prvStatsCommand,
    0
};

static const CLI_Command_Definition_t xPvdCommand = {
    "pvd",
    "\r\npvd [test | status]:\r\n test - simulate PVD trigger\r\n status - show PVD status\r\n",
    prvPvdCommand,
    -1
};

static const CLI_Command_Definition_t xShellCommand = {
    "shell",
    "\r\nshell:\r\n Displays system information (CPU, memory, chip info, etc.)\r\n",
    prvShellCommand,
    0
};

/*-----------------------------------------------------------*/

typedef struct {
    int target_lines;
    int is_all;
    int current_file_idx;
    uint32_t read_pos;
    uint32_t skip_count;
    int first_call;
    char filename[32];
} LogTailState_t;

static LogTailState_t s_log_tail_state;
static int s_in_tail_mode = 0;

static int prvLogTailOpenFile(int idx, const char *base_name, void **fp, uint32_t *file_size)
{
    char file_path[40];
    if (idx == 0) {
        snprintf(file_path, sizeof(file_path), "%s", base_name);
    } else {
        snprintf(file_path, sizeof(file_path), "%s.%d", base_name, idx);
    }
    
    *fp = elog_file_port_fopen(file_path, "r");
    if (*fp == NULL) {
        return -1;
    }
    
    if (elog_file_port_fseek(*fp, 0L, SEEK_END) != 0) {
        elog_file_port_fclose(*fp);
        return -1;
    }
    
    *file_size = elog_file_port_ftell(*fp);
    elog_file_port_fseek(*fp, 0L, SEEK_SET);
    return 0;
}

static int prvLogTailCountLines(void *fp, uint32_t file_size)
{
    uint32_t original_pos = elog_file_port_ftell(fp);
    elog_file_port_fseek(fp, 0L, SEEK_SET);
    
    #define COUNT_BUF_SIZE 256
    static uint8_t count_buf[COUNT_BUF_SIZE];
    int lines = 0;
    uint32_t bytes_read = 0;
    
    while (bytes_read < file_size) {
        uint32_t to_read = (file_size - bytes_read > COUNT_BUF_SIZE) ? COUNT_BUF_SIZE : (file_size - bytes_read);
        uint32_t read = elog_file_port_fread(count_buf, 1, to_read, fp);
        if (read == 0) break;
        
        for (uint32_t i = 0; i < read; i++) {
            if (count_buf[i] == '\n') lines++;
        }
        bytes_read += read;
    }
    
    elog_file_port_fseek(fp, original_pos, SEEK_SET);
    return lines;
}

static int prvLogTailReadBlock(void *fp, uint32_t file_size, uint32_t *read_pos, uint8_t *buf, uint32_t buf_size, uint32_t *data_len)
{
    *data_len = 0;
    if (*read_pos >= file_size) {
        return 0;
    }
    
    elog_file_port_fseek(fp, *read_pos, SEEK_SET);
    
    uint32_t to_read = (file_size - *read_pos > buf_size - 1) ? (buf_size - 1) : (file_size - *read_pos);
    uint32_t bytes_read = elog_file_port_fread(buf, 1, to_read, fp);
    
    if (bytes_read > 0) {
        buf[bytes_read] = '\0';
        *data_len = bytes_read;
        *read_pos += bytes_read;
        return 1;
    }
    
    return 0;
}

void vRegisterSampleCLICommands( void )
{
    FreeRTOS_CLIRegisterCommand( &xTaskStats );
    FreeRTOS_CLIRegisterCommand( &xThreeParameterEcho );
    FreeRTOS_CLIRegisterCommand( &xParameterEcho );

    #if ( configGENERATE_RUN_TIME_STATS == 1 )
    {
        FreeRTOS_CLIRegisterCommand( &xRunTimeStats );
    }
    #endif

    #if ( configINCLUDE_QUERY_HEAP_COMMAND == 1 )
    {
        FreeRTOS_CLIRegisterCommand( &xQueryHeap );
    }
    #endif

    #if ( configINCLUDE_TRACE_RELATED_CLI_COMMANDS == 1 )
    {
        FreeRTOS_CLIRegisterCommand( &xStartStopTrace );
    }
    #endif

    FreeRTOS_CLIRegisterCommand( &xTasksCommand );
    FreeRTOS_CLIRegisterCommand( &xHeapCommand );
    FreeRTOS_CLIRegisterCommand( &xFlashCommand );
    FreeRTOS_CLIRegisterCommand( &xLogCommand );
    FreeRTOS_CLIRegisterCommand( &xResetCommand );
    FreeRTOS_CLIRegisterCommand( &xStatsCommand );
    FreeRTOS_CLIRegisterCommand( &xPvdCommand );
    FreeRTOS_CLIRegisterCommand( &xShellCommand );
}
/*-----------------------------------------------------------*/

static BaseType_t prvShellCommand(char *pcWriteBuffer, size_t xWriteBufferLen, const char *pcCommandString)
{
    (void)pcCommandString;

    uint32_t ulTotalHeap = configTOTAL_HEAP_SIZE;
    uint32_t ulFreeHeap = xPortGetFreeHeapSize();
    uint32_t ulMinFreeHeap = xPortGetMinimumEverFreeHeapSize();
    uint32_t ulUsedHeap = ulTotalHeap - ulFreeHeap;

    size_t len = 0;

    if (len < xWriteBufferLen - 1) {
        int ret = snprintf(pcWriteBuffer + len, xWriteBufferLen - len, "\r\n--- System Information ---\r\n");
        len += (ret > 0) ? (size_t)ret : 0;
    }

    if (len < xWriteBufferLen - 1) {
        uint32_t dev_id = (DBGMCU->IDCODE) & DBGMCU_IDCODE_DEV_ID;
        uint32_t rev_id = ((DBGMCU->IDCODE) & DBGMCU_IDCODE_REV_ID) >> DBGMCU_IDCODE_REV_ID_Pos;
        int ret = snprintf(pcWriteBuffer + len, xWriteBufferLen - len,
                            "CPU: %s (DEV_ID:0x%03lX, REV_ID:0x%04lX)\r\n",
                            CPU_MODEL_STRING, (unsigned long)dev_id, (unsigned long)rev_id);
        len += (ret > 0) ? (size_t)ret : 0;
    }

    if (len < xWriteBufferLen - 1) {
        int ret = snprintf(pcWriteBuffer + len, xWriteBufferLen - len, "CPU Frequency: %lu MHz\r\n", SystemCoreClock / 1000000);
        len += (ret > 0) ? (size_t)ret : 0;
    }

    if (len < xWriteBufferLen - 1) {
        int ret = snprintf(pcWriteBuffer + len, xWriteBufferLen - len, "Architecture: ARM Cortex-M4\r\n");
        len += (ret > 0) ? (size_t)ret : 0;
    }

    if (len < xWriteBufferLen - 1) {
        int ret = snprintf(pcWriteBuffer + len, xWriteBufferLen - len, "FPU: %s\r\n", (__FPU_PRESENT == 1) ? "Enabled" : "Disabled");
        len += (ret > 0) ? (size_t)ret : 0;
    }

    if (len < xWriteBufferLen - 1) {
        int ret = snprintf(pcWriteBuffer + len, xWriteBufferLen - len, "\r\n--- Memory Usage ---\r\n");
        len += (ret > 0) ? (size_t)ret : 0;
    }

    if (len < xWriteBufferLen - 1) {
        int ret = snprintf(pcWriteBuffer + len, xWriteBufferLen - len, "Total Heap: %lu bytes\r\n", ulTotalHeap);
        len += (ret > 0) ? (size_t)ret : 0;
    }

    if (len < xWriteBufferLen - 1) {
        int ret = snprintf(pcWriteBuffer + len, xWriteBufferLen - len, "Used Heap: %lu bytes (%lu%%)\r\n", ulUsedHeap, (ulUsedHeap * 100) / ulTotalHeap);
        len += (ret > 0) ? (size_t)ret : 0;
    }

    if (len < xWriteBufferLen - 1) {
        int ret = snprintf(pcWriteBuffer + len, xWriteBufferLen - len, "Free Heap: %lu bytes\r\n", ulFreeHeap);
        len += (ret > 0) ? (size_t)ret : 0;
    }

    if (len < xWriteBufferLen - 1) {
        int ret = snprintf(pcWriteBuffer + len, xWriteBufferLen - len, "Min Ever Free Heap: %lu bytes\r\n", ulMinFreeHeap);
        len += (ret > 0) ? (size_t)ret : 0;
    }

    if (len < xWriteBufferLen - 1) {
        int ret = snprintf(pcWriteBuffer + len, xWriteBufferLen - len, "\r\n--- Operating System ---\r\n");
        len += (ret > 0) ? (size_t)ret : 0;
    }

    if (len < xWriteBufferLen - 1) {
        int ret = snprintf(pcWriteBuffer + len, xWriteBufferLen - len, "RTOS: FreeRTOS v%d.%d.%d\r\n",
                           (tskKERNEL_VERSION_MAJOR), (tskKERNEL_VERSION_MINOR), (tskKERNEL_VERSION_BUILD));
        len += (ret > 0) ? (size_t)ret : 0;
    }

    if (len < xWriteBufferLen - 1) {
        int ret = snprintf(pcWriteBuffer + len, xWriteBufferLen - len, "Tasks: %lu\r\n", (unsigned long)uxTaskGetNumberOfTasks());
        len += (ret > 0) ? (size_t)ret : 0;
    }

    if (len < xWriteBufferLen - 1) {
        int ret = snprintf(pcWriteBuffer + len, xWriteBufferLen - len, "Scheduler: %s\r\n", (xTaskGetSchedulerState() == taskSCHEDULER_RUNNING) ? "Running" : "Stopped");
        len += (ret > 0) ? (size_t)ret : 0;
    }

    if (len < xWriteBufferLen - 1) {
        int ret = snprintf(pcWriteBuffer + len, xWriteBufferLen - len, "\r\n--- Flash ---\r\n");
        len += (ret > 0) ? (size_t)ret : 0;
    }

    size_t flash_num = sfud_get_device_num();
    if (flash_num > 0) {
        for (size_t i = 0; i < flash_num; i++) {
            sfud_flash *flash_dev = sfud_get_device(i);
            if (flash_dev != NULL && len < xWriteBufferLen - 1) {
                int ret = snprintf(pcWriteBuffer + len, xWriteBufferLen - len, "Device %zu: %s\r\n", i, flash_dev->name);
                len += (ret > 0) ? (size_t)ret : 0;
                
                if (len < xWriteBufferLen - 1) {
                    ret = snprintf(pcWriteBuffer + len, xWriteBufferLen - len, "  Capacity: %lu bytes (%lu KB)\r\n",
                                   (unsigned long)flash_dev->chip.capacity, (unsigned long)(flash_dev->chip.capacity / 1024));
                    len += (ret > 0) ? (size_t)ret : 0;
                }
            }
        }
    } else if (len < xWriteBufferLen - 1) {
        snprintf(pcWriteBuffer + len, xWriteBufferLen - len, "Device: Not initialized\r\n");
    }

    pcWriteBuffer[xWriteBufferLen - 1] = '\0';

    return pdFALSE;
}
/*-----------------------------------------------------------*/

static char *prvWriteNameToBuffer( char *pcBuffer, const char *pcTaskName )
{
    size_t x;

    strncpy( pcBuffer, pcTaskName, ( size_t ) ( configMAX_TASK_NAME_LEN - 1 ) );
    pcBuffer[ configMAX_TASK_NAME_LEN - 1 ] = '\0';

    for( x = strlen( pcBuffer ); x < ( size_t ) ( configMAX_TASK_NAME_LEN - 1 ); x++ )
    {
        pcBuffer[ x ] = ' ';
    }

    pcBuffer[ x ] = ( char ) 0x00;

    return &( pcBuffer[ x + 1 ] );
}

static BaseType_t prvTaskStatsCommand( char * pcWriteBuffer,
                                       size_t xWriteBufferLen,
                                       const char * pcCommandString )
{
    const char * const pcHeader = "     State   Priority  Stack    #\r\n************************************************\r\n";
    BaseType_t xSpacePadding;
    static TaskStatus_t pxTaskStatusArray[ 16 ];
    UBaseType_t uxArraySize, x;
    char cStatus;
    size_t xRemainingSpace;

    ( void ) pcCommandString;
    configASSERT( pcWriteBuffer );

    xRemainingSpace = xWriteBufferLen;

    if( xRemainingSpace > 0 )
    {
        strncpy( pcWriteBuffer, "Task", xRemainingSpace - 1 );
        pcWriteBuffer[ xRemainingSpace - 1 ] = '\0';
        xRemainingSpace -= strlen( pcWriteBuffer );
        pcWriteBuffer += strlen( pcWriteBuffer );
    }

    configASSERT( configMAX_TASK_NAME_LEN > 3 );

    for( xSpacePadding = strlen( "Task" ); xSpacePadding < ( configMAX_TASK_NAME_LEN - 3 ) && xRemainingSpace > 1; xSpacePadding++ )
    {
        *pcWriteBuffer = ' ';
        pcWriteBuffer++;
        xRemainingSpace--;
    }

    if( xRemainingSpace > 0 )
    {
        *pcWriteBuffer = '\0';
    }

    if( xRemainingSpace > 1 && strlen( pcHeader ) < xRemainingSpace )
    {
        strncpy( pcWriteBuffer, pcHeader, xRemainingSpace - 1 );
        pcWriteBuffer[ xRemainingSpace - 1 ] = '\0';
        xRemainingSpace -= strlen( pcWriteBuffer );
        pcWriteBuffer += strlen( pcWriteBuffer );
    }

    uxArraySize = uxTaskGetSystemState( pxTaskStatusArray, 16, NULL );

    for( x = 0; x < uxArraySize && xRemainingSpace > 32; x++ )
    {
        char *pcOriginalBuffer = pcWriteBuffer;
        
        pcWriteBuffer = prvWriteNameToBuffer( pcWriteBuffer, pxTaskStatusArray[ x ].pcTaskName );
        xRemainingSpace -= ( size_t ) ( pcWriteBuffer - pcOriginalBuffer );

        switch( pxTaskStatusArray[ x ].eCurrentState )
        {
            case eRunning:     cStatus = 'X'; break;
            case eReady:       cStatus = 'R'; break;
            case eBlocked:     cStatus = 'B'; break;
            case eSuspended:   cStatus = 'S'; break;
            case eDeleted:     cStatus = 'D'; break;
            case eInvalid:     cStatus = ( char ) 0x00; break;
            default:           cStatus = ( char ) 0x00; break;
        }

        snprintf( pcWriteBuffer, xRemainingSpace, "\t%c\t%u\t%u\t%u\r\n",
                  cStatus,
                  ( unsigned int ) pxTaskStatusArray[ x ].uxCurrentPriority,
                  ( unsigned int ) pxTaskStatusArray[ x ].usStackHighWaterMark,
                  ( unsigned int ) pxTaskStatusArray[ x ].xTaskNumber );

        pcWriteBuffer += strlen( pcWriteBuffer );
        xRemainingSpace -= strlen( pcWriteBuffer );
    }

    return pdFALSE;
}
/*-----------------------------------------------------------*/

#if ( configINCLUDE_QUERY_HEAP_COMMAND == 1 )

    static BaseType_t prvQueryHeapCommand( char * pcWriteBuffer,
                                           size_t xWriteBufferLen,
                                           const char * pcCommandString )
    {
        ( void ) pcCommandString;
        ( void ) xWriteBufferLen;
        configASSERT( pcWriteBuffer );

        sprintf( pcWriteBuffer, "Current free heap %d bytes, minimum ever free heap %d bytes\r\n", ( int ) xPortGetFreeHeapSize(), ( int ) xPortGetMinimumEverFreeHeapSize() );

        return pdFALSE;
    }

#endif /* configINCLUDE_QUERY_HEAP */
/*-----------------------------------------------------------*/

#if ( configGENERATE_RUN_TIME_STATS == 1 )

    static BaseType_t prvRunTimeStatsCommand( char * pcWriteBuffer,
                                              size_t xWriteBufferLen,
                                              const char * pcCommandString )
    {
        const char * const pcHeader = "  Abs Time      % Time\r\n****************************************\r\n";
        BaseType_t xSpacePadding;
        static TaskStatus_t pxTaskStatusArray[ 16 ];
        UBaseType_t uxArraySize, x;
        unsigned long ulTotalTime, ulStatsAsPercentage;
        size_t xRemainingSpace;

        ( void ) pcCommandString;
        configASSERT( pcWriteBuffer );

        xRemainingSpace = xWriteBufferLen;

        if( xRemainingSpace > 0 )
        {
            strncpy( pcWriteBuffer, "Task", xRemainingSpace - 1 );
            pcWriteBuffer[ xRemainingSpace - 1 ] = '\0';
            xRemainingSpace -= strlen( pcWriteBuffer );
            pcWriteBuffer += strlen( pcWriteBuffer );
        }

        for( xSpacePadding = strlen( "Task" ); xSpacePadding < ( configMAX_TASK_NAME_LEN - 3 ) && xRemainingSpace > 1; xSpacePadding++ )
        {
            *pcWriteBuffer = ' ';
            pcWriteBuffer++;
            xRemainingSpace--;
        }

        if( xRemainingSpace > 0 )
        {
            *pcWriteBuffer = '\0';
        }

        if( xRemainingSpace > 1 && strlen( pcHeader ) < xRemainingSpace )
        {
            strncpy( pcWriteBuffer, pcHeader, xRemainingSpace - 1 );
            pcWriteBuffer[ xRemainingSpace - 1 ] = '\0';
            xRemainingSpace -= strlen( pcWriteBuffer );
            pcWriteBuffer += strlen( pcWriteBuffer );
        }

        uxArraySize = uxTaskGetSystemState( pxTaskStatusArray, 16, &ulTotalTime );

        if( ulTotalTime > 0UL )
        {
            ulTotalTime /= 100UL;

            for( x = 0; x < uxArraySize && xRemainingSpace > 32; x++ )
            {
                char *pcOriginalBuffer = pcWriteBuffer;
                
                pcWriteBuffer = prvWriteNameToBuffer( pcWriteBuffer, pxTaskStatusArray[ x ].pcTaskName );
                xRemainingSpace -= ( size_t ) ( pcWriteBuffer - pcOriginalBuffer );

                ulStatsAsPercentage = pxTaskStatusArray[ x ].ulRunTimeCounter / ulTotalTime;

                if( ulStatsAsPercentage > 0UL )
                {
                    snprintf( pcWriteBuffer, xRemainingSpace, "\t%lu\t\t%lu%%\r\n",
                              pxTaskStatusArray[ x ].ulRunTimeCounter, ulStatsAsPercentage );
                }
                else
                {
                    snprintf( pcWriteBuffer, xRemainingSpace, "\t%lu\t\t<1%%\r\n",
                              pxTaskStatusArray[ x ].ulRunTimeCounter );
                }

                pcWriteBuffer += strlen( pcWriteBuffer );
                xRemainingSpace -= strlen( pcWriteBuffer );
            }
        }

        return pdFALSE;
    }

#endif /* configGENERATE_RUN_TIME_STATS */
/*-----------------------------------------------------------*/

static BaseType_t prvThreeParameterEchoCommand( char * pcWriteBuffer,
                                                size_t xWriteBufferLen,
                                                const char * pcCommandString )
{
    const char * pcParameter;
    BaseType_t xParameterStringLength, xReturn;
    static UBaseType_t uxParameterNumber = 0;

    ( void ) pcCommandString;
    ( void ) xWriteBufferLen;
    configASSERT( pcWriteBuffer );

    if( uxParameterNumber == 0 )
    {
        sprintf( pcWriteBuffer, "The three parameters were:\r\n" );
        uxParameterNumber = 1U;
        xReturn = pdPASS;
    }
    else
    {
        pcParameter = FreeRTOS_CLIGetParameter
                      (
            pcCommandString,
            uxParameterNumber,
            &xParameterStringLength
                      );

        configASSERT( pcParameter );

        memset( pcWriteBuffer, 0x00, xWriteBufferLen );
        sprintf( pcWriteBuffer, "%d: ", ( int ) uxParameterNumber );
        strncat( pcWriteBuffer, pcParameter, ( size_t ) xParameterStringLength );
        strcat( pcWriteBuffer, "\r\n" );

        if( uxParameterNumber == 3U )
        {
            xReturn = pdFALSE;
            uxParameterNumber = 0;
        }
        else
        {
            xReturn = pdTRUE;
            uxParameterNumber++;
        }
    }

    return xReturn;
}
/*-----------------------------------------------------------*/

static BaseType_t prvParameterEchoCommand( char * pcWriteBuffer,
                                           size_t xWriteBufferLen,
                                           const char * pcCommandString )
{
    const char * pcParameter;
    BaseType_t xParameterStringLength, xReturn;
    static UBaseType_t uxParameterNumber = 0;

    ( void ) pcCommandString;
    ( void ) xWriteBufferLen;
    configASSERT( pcWriteBuffer );

    if( uxParameterNumber == 0 )
    {
        sprintf( pcWriteBuffer, "The parameters were:\r\n" );
        uxParameterNumber = 1U;
        xReturn = pdPASS;
    }
    else
    {
        pcParameter = FreeRTOS_CLIGetParameter
                      (
            pcCommandString,
            uxParameterNumber,
            &xParameterStringLength
                      );

        if( pcParameter != NULL )
        {
            memset( pcWriteBuffer, 0x00, xWriteBufferLen );
            sprintf( pcWriteBuffer, "%d: ", ( int ) uxParameterNumber );
            strncat( pcWriteBuffer, ( char * ) pcParameter, ( size_t ) xParameterStringLength );
            strcat( pcWriteBuffer, "\r\n" );

            xReturn = pdTRUE;
            uxParameterNumber++;
        }
        else
        {
            pcWriteBuffer[ 0 ] = 0x00;
            xReturn = pdFALSE;
            uxParameterNumber = 0;
        }
    }

    return xReturn;
}
/*-----------------------------------------------------------*/

#if configINCLUDE_TRACE_RELATED_CLI_COMMANDS == 1

    static BaseType_t prvStartStopTraceCommand( char * pcWriteBuffer,
                                                size_t xWriteBufferLen,
                                                const char * pcCommandString )
    {
        const char * pcParameter;
        BaseType_t lParameterStringLength;

        ( void ) pcCommandString;
        ( void ) xWriteBufferLen;
        configASSERT( pcWriteBuffer );

        pcParameter = FreeRTOS_CLIGetParameter
                      (
            pcCommandString,
            1,
            &lParameterStringLength
                      );

        configASSERT( pcParameter );

        if( strncmp( pcParameter, "start", strlen( "start" ) ) == 0 )
        {
            vTraceStop();
            vTraceClear();
            vTraceStart();

            sprintf( pcWriteBuffer, "Trace recording (re)started.\r\n" );
        }
        else if( strncmp( pcParameter, "stop", strlen( "stop" ) ) == 0 )
        {
            vTraceStop();
            sprintf( pcWriteBuffer, "Stopping trace recording.\r\n" );
        }
        else
        {
            sprintf( pcWriteBuffer, "Valid parameters are 'start' and 'stop'.\r\n" );
        }

        return pdFALSE;
    }

#endif /* configINCLUDE_TRACE_RELATED_CLI_COMMANDS */
/*-----------------------------------------------------------*/

/* Project specific command implementations. */
static BaseType_t prvTasksCommand(char *pcWriteBuffer, size_t xWriteBufferLen, const char *pcCommandString)
{
    (void)pcCommandString;

    const char *pcHeader = "\r\nName          State  Priority  Stack  Num\r\n";
    strncpy(pcWriteBuffer, pcHeader, xWriteBufferLen - 1);
    pcWriteBuffer[xWriteBufferLen - 1] = '\0';
    
    size_t header_len = strlen(pcWriteBuffer);
    if (header_len < xWriteBufferLen - 1) {
        vTaskList(pcWriteBuffer + header_len);
        pcWriteBuffer[xWriteBufferLen - 1] = '\0';
    }

    return pdFALSE;
}

static BaseType_t prvHeapCommand(char *pcWriteBuffer, size_t xWriteBufferLen, const char *pcCommandString)
{
    (void)pcCommandString;

    snprintf(pcWriteBuffer, xWriteBufferLen, "\r\nCurrent free heap: %u bytes\r\n", (unsigned int)xPortGetFreeHeapSize());
    size_t len = strlen(pcWriteBuffer);
    if (len < xWriteBufferLen - 1) {
        snprintf(pcWriteBuffer + len, xWriteBufferLen - len, "Minimum ever free heap: %u bytes\r\n", (unsigned int)xPortGetMinimumEverFreeHeapSize());
    }

    return pdFALSE;
}

static BaseType_t prvFlashCommand(char *pcWriteBuffer, size_t xWriteBufferLen, const char *pcCommandString)
{
    (void)pcCommandString;
    size_t len = 0;

    elog_file_port_lock();
    lfs_t *pLfs = elog_file_port_get_lfs();
    struct lfs_config *pLfsCfg = elog_file_port_get_lfs_config();

    if (pLfs != NULL && pLfsCfg != NULL) {
        snprintf(pcWriteBuffer, xWriteBufferLen, "\r\n--- Files in Flash ---\r\n");
        len = strlen(pcWriteBuffer);

        lfs_dir_t dir;
        int err = lfs_dir_open(pLfs, &dir, "/");
        if (err == LFS_ERR_OK) {
            struct lfs_info info;
            int file_count = 0;
            while (lfs_dir_read(pLfs, &dir, &info) > 0) {
                if ((strcmp(info.name, ".") == 0) || (strcmp(info.name, "..") == 0)) {
                    continue;
                }
                if (info.type == LFS_TYPE_REG) {
                    if (len < xWriteBufferLen - 1) {
                        int ret = snprintf(pcWriteBuffer + len, xWriteBufferLen - len, "File: %-16.16s Size: %lu bytes\r\n", info.name, (unsigned long)info.size);
                        len += (ret > 0) ? (size_t)ret : 0;
                    }
                    file_count++;
                } else if (info.type == LFS_TYPE_DIR) {
                    if (len < xWriteBufferLen - 1) {
                        int ret = snprintf(pcWriteBuffer + len, xWriteBufferLen - len, "Dir:  %.50s\r\n", info.name);
                        len += (ret > 0) ? (size_t)ret : 0;
                    }
                }
            }
            lfs_dir_close(pLfs, &dir);
            if (file_count == 0 && len < xWriteBufferLen - 1) {
                int ret = snprintf(pcWriteBuffer + len, xWriteBufferLen - len, "No files found\r\n");
                len += (ret > 0) ? (size_t)ret : 0;
            }
        } else {
            snprintf(pcWriteBuffer, xWriteBufferLen, "\r\nFailed to open directory, err=%d\r\n", err);
            len = strlen(pcWriteBuffer);
        }

        lfs_ssize_t used_blocks = lfs_fs_size(pLfs);
        if (used_blocks >= 0) {
            uint32_t used_bytes = (uint32_t)used_blocks * pLfsCfg->block_size;
            uint32_t total_bytes = pLfsCfg->block_count * pLfsCfg->block_size;
            uint32_t free_bytes = total_bytes - used_bytes;
            float used_percent = ((float)used_bytes / total_bytes) * 100;

            if (len < xWriteBufferLen - 1) {
                int ret = snprintf(pcWriteBuffer + len, xWriteBufferLen - len, "\r\n--- Flash Usage ---\r\nUsed: %lu bytes (%lu blocks)\r\n", used_bytes, (uint32_t)used_blocks);
                len += (ret > 0) ? (size_t)ret : 0;
            }
            if (len < xWriteBufferLen - 1) {
                int ret = snprintf(pcWriteBuffer + len, xWriteBufferLen - len, "Free: %lu bytes\r\nTotal: %lu bytes\r\nUsage: %.1f%%\r\n", free_bytes, total_bytes, used_percent);
                len += (ret > 0) ? (size_t)ret : 0;
            }
        } else {
            if (len < xWriteBufferLen - 1) {
                snprintf(pcWriteBuffer + len, xWriteBufferLen - len, "\r\nFailed to get flash usage, err=%ld\r\n", (long)used_blocks);
            }
        }
    } else {
        snprintf(pcWriteBuffer, xWriteBufferLen, "\r\nFlash filesystem not initialized\r\n");
    }
    elog_file_port_unlock();

    pcWriteBuffer[xWriteBufferLen - 1] = '\0';
    return pdFALSE;
}

static BaseType_t prvLogCommand(char *pcWriteBuffer, size_t xWriteBufferLen, const char *pcCommandString)
{
    const char *pcParameter;
    BaseType_t xParameterStringLength;

    if (!s_in_tail_mode) {
        pcParameter = FreeRTOS_CLIGetParameter(pcCommandString, 1, &xParameterStringLength);

        if (pcParameter == NULL) {
            snprintf(pcWriteBuffer, xWriteBufferLen, "\r\nlog: Missing parameter. Use 'log level <lvl>' or 'log tail'\r\n");
            return pdFALSE;
        }

        if (strncmp(pcParameter, "level", 5) == 0) {
            const char *pcLevel = FreeRTOS_CLIGetParameter(pcCommandString, 2, &xParameterStringLength);
            if (pcLevel == NULL) {
                snprintf(pcWriteBuffer, xWriteBufferLen, "\r\nlog level: Missing level parameter (v/d/i/w/e)\r\n");
            } else {
                switch (pcLevel[0]) {
                    case 'v':
                    case 'V':
                        elog_set_filter_lvl(ELOG_LVL_VERBOSE);
                        snprintf(pcWriteBuffer, xWriteBufferLen, "\r\nLog level set to VERBOSE\r\n");
                        break;
                    case 'd':
                    case 'D':
                        elog_set_filter_lvl(ELOG_LVL_DEBUG);
                        snprintf(pcWriteBuffer, xWriteBufferLen, "\r\nLog level set to DEBUG\r\n");
                        break;
                    case 'i':
                    case 'I':
                        elog_set_filter_lvl(ELOG_LVL_INFO);
                        snprintf(pcWriteBuffer, xWriteBufferLen, "\r\nLog level set to INFO\r\n");
                        break;
                    case 'w':
                    case 'W':
                        elog_set_filter_lvl(ELOG_LVL_WARN);
                        snprintf(pcWriteBuffer, xWriteBufferLen, "\r\nLog level set to WARN\r\n");
                        break;
                    case 'e':
                    case 'E':
                        elog_set_filter_lvl(ELOG_LVL_ERROR);
                        snprintf(pcWriteBuffer, xWriteBufferLen, "\r\nLog level set to ERROR\r\n");
                        break;
                    default:
                        snprintf(pcWriteBuffer, xWriteBufferLen, "\r\nInvalid log level. Valid levels: v(verbose), d(debug), i(info), w(warn), e(error)\r\n");
                        break;
                }
            }
            return pdFALSE;
        } else if (strncmp(pcParameter, "tail", 4) == 0) {
            const char *pcCount = FreeRTOS_CLIGetParameter(pcCommandString, 2, &xParameterStringLength);
            
            if (pcCount == NULL) {
                snprintf(s_log_tail_state.filename, sizeof(s_log_tail_state.filename), "log.txt");
                s_log_tail_state.target_lines = 10;
                s_log_tail_state.is_all = 0;
                s_log_tail_state.current_file_idx = 0;
                s_log_tail_state.read_pos = 0;
                s_log_tail_state.skip_count = 0;
                s_log_tail_state.first_call = 1;
                s_in_tail_mode = 1;
                
                snprintf(pcWriteBuffer, xWriteBufferLen, "\r\n=== Output: Last 10 lines from log.txt ===\r\n");
                return pdTRUE;
            }
            
            if (strncmp(pcCount, "all", 3) == 0) {
                const char *pcFile = FreeRTOS_CLIGetParameter(pcCommandString, 3, &xParameterStringLength);
                if (pcFile != NULL) {
                    snprintf(s_log_tail_state.filename, sizeof(s_log_tail_state.filename), "%.*s", (int)xParameterStringLength, pcFile);
                } else {
                    snprintf(s_log_tail_state.filename, sizeof(s_log_tail_state.filename), "log.txt");
                }
                
                s_log_tail_state.target_lines = -1;
                s_log_tail_state.is_all = 1;
                s_log_tail_state.current_file_idx = 0;
                s_log_tail_state.read_pos = 0;
                s_log_tail_state.skip_count = 0;
                s_log_tail_state.first_call = 1;
                s_in_tail_mode = 1;
                
                snprintf(pcWriteBuffer, xWriteBufferLen, "\r\n=== Output: All lines from %s ===\r\n", s_log_tail_state.filename);
                return pdTRUE;
            }
            
            char *endptr;
            long count = strtol(pcCount, &endptr, 10);
            if (count <= 0) {
                snprintf(pcWriteBuffer, xWriteBufferLen, "\r\nInvalid count: %s\r\n", pcCount);
                return pdFALSE;
            }
            
            const char *pcFile = FreeRTOS_CLIGetParameter(pcCommandString, 3, &xParameterStringLength);
            if (pcFile != NULL) {
                snprintf(s_log_tail_state.filename, sizeof(s_log_tail_state.filename), "%.*s", (int)xParameterStringLength, pcFile);
            } else {
                snprintf(s_log_tail_state.filename, sizeof(s_log_tail_state.filename), "log.txt");
            }
            
            s_log_tail_state.target_lines = (int)count;
            s_log_tail_state.is_all = 0;
            s_log_tail_state.current_file_idx = 0;
            s_log_tail_state.read_pos = 0;
            s_log_tail_state.skip_count = 0;
            s_log_tail_state.first_call = 1;
            s_in_tail_mode = 1;
            
            snprintf(pcWriteBuffer, xWriteBufferLen, "\r\n=== Output: Last %d lines from %s ===\r\n", (int)count, s_log_tail_state.filename);
            return pdTRUE;
        } else {
            snprintf(pcWriteBuffer, xWriteBufferLen, "\r\nUnknown log command. Use 'log level <lvl>' or 'log tail'\r\n");
            return pdFALSE;
        }
    }
    
    if (s_log_tail_state.first_call == 2) {
        snprintf(pcWriteBuffer, xWriteBufferLen, "\r\n=== End of output ===\r\n");
        s_in_tail_mode = 0;
        return pdFALSE;
    }
    
    elog_file_port_lock();
    
    void *fp = NULL;
    uint32_t file_size = 0;
    int result = -1;
    
    while (1) {
        result = prvLogTailOpenFile(s_log_tail_state.current_file_idx, s_log_tail_state.filename, &fp, &file_size);
        if (result == 0) {
            break;
        }
        
        s_log_tail_state.current_file_idx++;
        if (s_log_tail_state.current_file_idx > 10) {
            elog_file_port_unlock();
            snprintf(pcWriteBuffer, xWriteBufferLen, "\r\n=== End of output ===\r\n");
            s_in_tail_mode = 0;
            return pdFALSE;
        }
    }
    
    if (s_log_tail_state.is_all) {
        #define READ_BUF_SIZE 512
        static uint8_t read_buf[READ_BUF_SIZE];
        uint32_t data_len = 0;
        
        if (prvLogTailReadBlock(fp, file_size, &s_log_tail_state.read_pos, read_buf, READ_BUF_SIZE, &data_len) > 0) {
            strncpy(pcWriteBuffer, (char *)read_buf, xWriteBufferLen - 1);
            pcWriteBuffer[xWriteBufferLen - 1] = '\0';
            elog_file_port_fclose(fp);
            elog_file_port_unlock();
            return pdTRUE;
        } else {
            elog_file_port_fclose(fp);
            s_log_tail_state.current_file_idx++;
            
            void *next_fp = NULL;
            uint32_t next_file_size = 0;
            int next_result = prvLogTailOpenFile(s_log_tail_state.current_file_idx, s_log_tail_state.filename, &next_fp, &next_file_size);
            
            if (next_result != 0 || s_log_tail_state.current_file_idx > 10) {
                elog_file_port_unlock();
                snprintf(pcWriteBuffer, xWriteBufferLen, "\r\n=== End of output ===\r\n");
                s_in_tail_mode = 0;
                return pdFALSE;
            }
            
            elog_file_port_fclose(next_fp);
            s_log_tail_state.read_pos = 0;
            
            snprintf(pcWriteBuffer, xWriteBufferLen, "\r\n=== Continuing from %s.%d ===\r\n", s_log_tail_state.filename, s_log_tail_state.current_file_idx);
            elog_file_port_unlock();
            return pdTRUE;
        }
    }
    
    #define READ_BUF_SIZE 512
    static uint8_t read_buf[READ_BUF_SIZE];
    uint32_t data_len = 0;
    size_t output_len = 0;
    
    if (s_log_tail_state.first_call) {
        int file_lines = prvLogTailCountLines(fp, file_size);
        
        if (file_lines <= s_log_tail_state.target_lines) {
            s_log_tail_state.skip_count = 0;
            s_log_tail_state.target_lines -= file_lines;
        } else {
            s_log_tail_state.skip_count = file_lines - s_log_tail_state.target_lines;
            s_log_tail_state.target_lines = 0;
        }
        
        s_log_tail_state.read_pos = 0;
        s_log_tail_state.first_call = 0;
    }
    
    if (s_log_tail_state.skip_count > 0) {
        #define SKIP_BUF_SIZE 256
        static uint8_t skip_buf[SKIP_BUF_SIZE];
        
        elog_file_port_fseek(fp, s_log_tail_state.read_pos, SEEK_SET);
        
        while (s_log_tail_state.skip_count > 0 && s_log_tail_state.read_pos < file_size) {
            uint32_t to_read = (file_size - s_log_tail_state.read_pos > SKIP_BUF_SIZE) ? SKIP_BUF_SIZE : (file_size - s_log_tail_state.read_pos);
            uint32_t bytes_read = elog_file_port_fread(skip_buf, 1, to_read, fp);
            if (bytes_read == 0) break;
            
            for (uint32_t i = 0; i < bytes_read; i++) {
                if (skip_buf[i] == '\n') {
                    s_log_tail_state.skip_count--;
                    if (s_log_tail_state.skip_count == 0) {
                        s_log_tail_state.read_pos += i + 1;
                        break;
                    }
                }
            }
            if (s_log_tail_state.skip_count > 0) {
                s_log_tail_state.read_pos += bytes_read;
            }
        }
    }
    
    elog_file_port_fseek(fp, s_log_tail_state.read_pos, SEEK_SET);
    
    while (data_len < READ_BUF_SIZE - 1 && s_log_tail_state.read_pos < file_size) {
        uint32_t to_read = READ_BUF_SIZE - 1 - data_len;
        if (file_size - s_log_tail_state.read_pos < to_read) {
            to_read = file_size - s_log_tail_state.read_pos;
        }
        
        uint32_t bytes_read = elog_file_port_fread(read_buf + data_len, 1, to_read, fp);
        if (bytes_read == 0) break;
        
        data_len += bytes_read;
        s_log_tail_state.read_pos += bytes_read;
    }
    
    elog_file_port_fclose(fp);
    
    if (data_len > 0) {
        read_buf[data_len] = '\0';
        strncpy(pcWriteBuffer, (char *)read_buf, xWriteBufferLen - 1);
        output_len = strlen(pcWriteBuffer);
        pcWriteBuffer[xWriteBufferLen - 1] = '\0';
    } else {
        pcWriteBuffer[0] = '\0';
        output_len = 0;
    }
    
    if (s_log_tail_state.read_pos >= file_size) {
        if (s_log_tail_state.target_lines > 0) {
            s_log_tail_state.current_file_idx++;
            
            void *next_fp = NULL;
            uint32_t next_file_size = 0;
            int next_result = prvLogTailOpenFile(s_log_tail_state.current_file_idx, s_log_tail_state.filename, &next_fp, &next_file_size);
            
            if (next_result != 0 || s_log_tail_state.current_file_idx > 10) {
                elog_file_port_unlock();
                strncat(pcWriteBuffer, "\r\n=== End of output ===\r\n", xWriteBufferLen - output_len - 1);
                s_in_tail_mode = 0;
                return pdFALSE;
            }
            
            int lines_in_next = prvLogTailCountLines(next_fp, next_file_size);
            elog_file_port_fclose(next_fp);
            
            if (lines_in_next <= s_log_tail_state.target_lines) {
                s_log_tail_state.skip_count = 0;
                s_log_tail_state.target_lines -= lines_in_next;
            } else {
                s_log_tail_state.skip_count = lines_in_next - s_log_tail_state.target_lines;
                s_log_tail_state.target_lines = 0;
            }
            
            s_log_tail_state.read_pos = 0;
            s_log_tail_state.first_call = 0;
            
            char continue_msg[80];
            snprintf(continue_msg, sizeof(continue_msg), "\r\n=== Continuing from %s.%d ===\r\n", s_log_tail_state.filename, s_log_tail_state.current_file_idx);
            strncat(pcWriteBuffer, continue_msg, xWriteBufferLen - output_len - 1);
        } else {
            elog_file_port_unlock();
            s_log_tail_state.first_call = 2;
            return pdTRUE;
        }
    }
    
    elog_file_port_unlock();
    return pdTRUE;
}

static BaseType_t prvResetCommand(char *pcWriteBuffer, size_t xWriteBufferLen, const char *pcCommandString)
{
    (void)pcCommandString;

    snprintf(pcWriteBuffer, xWriteBufferLen, "\r\nSystem resetting...\r\n");

    osDelay(200);
    NVIC_SystemReset();

    return pdFALSE;
}

static BaseType_t prvStatsCommand(char *pcWriteBuffer, size_t xWriteBufferLen, const char *pcCommandString)
{
    (void)pcCommandString;

    uart_rb_stats_t stats;
    uart_rb_get_stats(&stats);

    size_t len = 0;
    
    if (len < xWriteBufferLen - 1) {
        int ret = snprintf(pcWriteBuffer + len, xWriteBufferLen - len, "\r\n--- UART Ring Buffer Stats ---\r\n");
        len += (ret > 0) ? (size_t)ret : 0;
    }
    if (len < xWriteBufferLen - 1) {
        int ret = snprintf(pcWriteBuffer + len, xWriteBufferLen - len, "Total written: %lu bytes\r\n", stats.total_written);
        len += (ret > 0) ? (size_t)ret : 0;
    }
    if (len < xWriteBufferLen - 1) {
        int ret = snprintf(pcWriteBuffer + len, xWriteBufferLen - len, "Total dropped: %lu bytes\r\n", stats.total_dropped);
        len += (ret > 0) ? (size_t)ret : 0;
    }
    if (len < xWriteBufferLen - 1) {
        int ret = snprintf(pcWriteBuffer + len, xWriteBufferLen - len, "DMA TX count: %lu\r\n", stats.dma_tx_count);
        len += (ret > 0) ? (size_t)ret : 0;
    }
    if (len < xWriteBufferLen - 1) {
        int ret = snprintf(pcWriteBuffer + len, xWriteBufferLen - len, "Buffer full count: %lu\r\n", stats.buffer_full_count);
        len += (ret > 0) ? (size_t)ret : 0;
    }
    if (len < xWriteBufferLen - 1) {
        int ret = snprintf(pcWriteBuffer + len, xWriteBufferLen - len, "Current used: %lu bytes\r\n", stats.current_used);
        len += (ret > 0) ? (size_t)ret : 0;
    }

    pcWriteBuffer[xWriteBufferLen - 1] = '\0';
    return pdFALSE;
}

static BaseType_t prvPvdCommand(char *pcWriteBuffer, size_t xWriteBufferLen, const char *pcCommandString)
{
    const char *pcParameter;
    BaseType_t xParameterStringLength;
    size_t len = 0;

    pcParameter = FreeRTOS_CLIGetParameter(pcCommandString, 1, &xParameterStringLength);

    if (pcParameter == NULL) {
        snprintf(pcWriteBuffer, xWriteBufferLen, "\r\nPVD: Missing parameter. Use 'pvd status' or 'pvd test'\r\n");
        return pdFALSE;
    }

    if (strncmp(pcParameter, "status", 6) == 0) {
        if (len < xWriteBufferLen - 1) {
            int ret = snprintf(pcWriteBuffer + len, xWriteBufferLen - len, "\r\n--- PVD Status ---\r\n");
            len += (ret > 0) ? (size_t)ret : 0;
        }
        if (len < xWriteBufferLen - 1) {
            int ret = snprintf(pcWriteBuffer + len, xWriteBufferLen - len, "Trigger count: %u\r\n", (unsigned int)g_pvd_trigger_count);
            len += (ret > 0) ? (size_t)ret : 0;
        }
        if (len < xWriteBufferLen - 1) {
            int ret = snprintf(pcWriteBuffer + len, xWriteBufferLen - len, "Power failure: %s\r\n", g_power_failure ? "Yes" : "No");
            len += (ret > 0) ? (size_t)ret : 0;
        }
        if (len < xWriteBufferLen - 1) {
            int ret = snprintf(pcWriteBuffer + len, xWriteBufferLen - len, "PVDO flag: %s\r\n", __HAL_PWR_GET_FLAG(PWR_FLAG_PVDO) ? "Set (VDD < threshold)" : "Not set (VDD >= threshold)");
            len += (ret > 0) ? (size_t)ret : 0;
        }
    } else if (strncmp(pcParameter, "test", 4) == 0) {
        snprintf(pcWriteBuffer, xWriteBufferLen, "\r\nPVD simulation starting...\r\n");
        vTaskDelay(pdMS_TO_TICKS(100));
        pvd_simulate_trigger_with_power_fail();
    } else {
        snprintf(pcWriteBuffer, xWriteBufferLen, "\r\nUnknown PVD command. Use 'pvd status' or 'pvd test'\r\n");
    }

    pcWriteBuffer[xWriteBufferLen - 1] = '\0';
    return pdFALSE;
}
/*-----------------------------------------------------------*/