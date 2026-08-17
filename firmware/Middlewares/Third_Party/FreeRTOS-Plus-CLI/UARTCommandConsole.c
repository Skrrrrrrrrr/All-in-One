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
 * Modified for STM32F4 + HAL + FreeRTOS platform
 * - Uses uart_ringbuf for DMA output
 * - Added command history buffer (up/down arrows)
 * - Added cursor movement (left/right arrows)
 * - Added ANSI escape sequence handling
 */

#include "string.h"
#include "stdio.h"

#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"

#include "FreeRTOS_CLI.h"
#include "serial.h"
#include "uart_ringbuf.h"

#define cmdMAX_INPUT_SIZE          64
#define cmdHISTORY_SIZE            5
#define cmdQUEUE_LENGTH            512
#define cmdASCII_DEL               ( 0x7F )
#define cmdMAX_MUTEX_WAIT          pdMS_TO_TICKS( 300 )
#define cmdPROMPT_STRING           "> "

#ifndef configCLI_BAUD_RATE
    #define configCLI_BAUD_RATE    115200
#endif

static void prvUARTCommandConsoleTask( void * pvParameters );
void vUARTCommandConsoleStart( uint16_t usStackSize, UBaseType_t uxPriority );
void vOutputString( const char * const pcMessage );
void vRefreshCLIPrompt( void );
void vSetPromptRefreshNeeded( void );

static const char * const pcWelcomeMessage = "CLI task started, type 'help' for available commands\r\n";
static const char * const pcEndOfOutputMessage = "\r\n[End of command output]\r\n";
static const char * const pcNewLine = "\r\n";

volatile BaseType_t xPromptRefreshNeeded = pdFALSE;
volatile BaseType_t xPromptDisplayed = pdFALSE;
volatile BaseType_t xInUserInputMode = pdFALSE;

xComPortHandle xPort = 0;

/* 输入/历史缓冲仅由 CLI 任务 CPU 访问（UART RX DMA 写入的是 usart.c 的
 * rx_buff，TX DMA 读取的是 uart_ringbuf.c 的 uart_rb），可安全放入 CCMRAM。 */
volatile char cInputString[ cmdMAX_INPUT_SIZE ] __attribute__((section(".bss.cInputString")));
volatile uint8_t ucInputIndex = 0;

/* ---------- History buffer ---------- */
static char cHistoryBuffer[ cmdHISTORY_SIZE ][ cmdMAX_INPUT_SIZE ] __attribute__((section(".bss.cHistoryBuffer")));
static uint8_t ucHistoryCount = 0;      /* Number of valid history entries */
static uint8_t ucHistoryBrowse = 0;     /* 0=not browsing; 1..N = browsing from newest */
static char cSavedInput[ cmdMAX_INPUT_SIZE ] __attribute__((section(".bss.cSavedInput")));  /* Saved partial input when starting browse */

/* ---------- Cursor position ---------- */
static uint8_t ucCursorPos = 0;

/* ---------- Escape sequence state ---------- */
static uint8_t ucEscapeState = 0;       /* 0=normal, 1=ESC, 2=ESC[ */

/* ------------------------------------------------
 * History helpers
 * ------------------------------------------------ */
static void vAddToHistory( const char *cmd )
{
    if( cmd == NULL || cmd[0] == '\0' ) return;

    /* Skip duplicate of the newest entry */
    if( ucHistoryCount > 0 && strcmp( cHistoryBuffer[0], cmd ) == 0 ) return;

    /* Shift entries down (newest at index 0) */
    if( ucHistoryCount < cmdHISTORY_SIZE ) ucHistoryCount++;
    uint8_t i;
    for( i = ucHistoryCount - 1; i > 0; i-- )
    {
        strcpy( cHistoryBuffer[i], cHistoryBuffer[i-1] );
    }
    strcpy( cHistoryBuffer[0], cmd );
}

static void vLoadHistory( uint8_t index )
{
    strcpy( ( void * ) cInputString, cHistoryBuffer[index] );
    ucInputIndex = ( uint8_t ) strlen( ( const char * ) cInputString );
    ucCursorPos = ucInputIndex;
}

/* Redraw the entire input line after cursor/history change.
 * Uses ANSI ESC[2K to clear the entire line first, avoiding the
 * visible flicker that \r causes when the cursor jumps to column 0. */
static void vRedrawInputLine( void )
{
    /* Clear entire line then go to column 0 (ESC[2K\r) */
    uart_rb_write( ( uint8_t * ) "\x1B[2K\r", sizeof("\x1B[2K\r") );

    /* Write prompt + new text */
    uart_rb_write( ( uint8_t * ) cmdPROMPT_STRING, strlen( cmdPROMPT_STRING ) );
    uart_rb_write( ( uint8_t * ) cInputString, ucInputIndex );

    /* Move cursor back to correct visual position */
    uint8_t moveBack = ucInputIndex - ucCursorPos;
    while( moveBack > 0 )
    {
        uart_rb_write( ( uint8_t * ) "\b", sizeof("\b") );
        moveBack--;
    }
}

/* ------------------------------------------------
 * Arrow key handlers (call inside mutex)
 * ------------------------------------------------ */
static void prvHandleUp( void )
{
    if( ucHistoryCount == 0 ) return;

    if( ucHistoryBrowse == 0 )
    {
        /* First time – save current partial input */
        strcpy( cSavedInput, ( void * ) cInputString );
        ucHistoryBrowse = 1;
    }
    else if( ucHistoryBrowse < ucHistoryCount )
    {
        ucHistoryBrowse++;
    }
    else
    {
        return; /* Already at oldest */
    }

    vLoadHistory( ucHistoryBrowse - 1 );
    vRedrawInputLine();
}

static void prvHandleDown( void )
{
    if( ucHistoryBrowse == 0 ) return;

    ucHistoryBrowse--;
    if( ucHistoryBrowse == 0 )
    {
        /* Restore saved input */
        strcpy( ( void * ) cInputString, cSavedInput );
        ucInputIndex = ( uint8_t ) strlen( ( const char * ) cInputString );
        ucCursorPos = ucInputIndex;
    }
    else
    {
        vLoadHistory( ucHistoryBrowse - 1 );
    }

    vRedrawInputLine();
}

static void prvHandleRight( void )
{
    if( ucCursorPos < ucInputIndex )
    {
        uart_rb_write( ( uint8_t * ) &cInputString[ ucCursorPos ], sizeof(char) );
        ucCursorPos++;
    }
}

static void prvHandleLeft( void )
{
    if( ucCursorPos > 0 )
    {
        uart_rb_write( ( uint8_t * ) "\b", sizeof("\b") );
        ucCursorPos--;
    }
}

/* ------------------------------------------------
 * Public API
 * ------------------------------------------------ */
void vUARTCommandConsoleStart( uint16_t usStackSize, UBaseType_t uxPriority )
{
    xTaskCreate( prvUARTCommandConsoleTask, "CLI", usStackSize, NULL, uxPriority, NULL );
}

static void prvUARTCommandConsoleTask( void * pvParameters )
{
    signed char cRxedChar;
    char * pcOutputString;
    BaseType_t xReturned;

    ( void ) pvParameters;

    pcOutputString = FreeRTOS_CLIGetOutputBuffer();
    xPort = xSerialPortInitMinimal( configCLI_BAUD_RATE, cmdQUEUE_LENGTH );
    if( xPort == 0 ) { for( ;; ); }

    uart_rb_write( ( uint8_t * ) pcWelcomeMessage, strlen( pcWelcomeMessage ) );
    uart_rb_write( ( uint8_t * ) cmdPROMPT_STRING, strlen( cmdPROMPT_STRING ) );
    xPromptDisplayed = pdTRUE;
    xInUserInputMode = pdTRUE;
    ucCursorPos = 0;

    for( ; ; )
    {
        if( xSerialGetChar( xPort, &cRxedChar, pdMS_TO_TICKS( 10 ) ) == pdPASS )
        {
            if( ucEscapeState == 0 )
            {
                if( cRxedChar == 0x1B ) { ucEscapeState = 1; continue; }
            }
            else if( ucEscapeState == 1 )
            {
                if( cRxedChar == '[' )   { ucEscapeState = 2; continue; }
                else                     { ucEscapeState = 0; continue; }
            }
            else if( ucEscapeState == 2 )
            {
                ucEscapeState = 0;
                switch( cRxedChar )
                {
                    case 'A': prvHandleUp();     break;
                    case 'B': prvHandleDown();   break;
                    case 'C': prvHandleRight();  break;
                    case 'D': prvHandleLeft();   break;
                    default:  break;
                }
                continue;
            }

            if( ( cRxedChar == '\n' ) || ( cRxedChar == '\r' ) )
            {
                xInUserInputMode = pdFALSE;
                uart_rb_write( ( uint8_t * ) pcNewLine, strlen( pcNewLine ) );

                if( ucInputIndex == 0 )
                {
                    /* 空白行回车：不执行任何指令，仅换行并打印新提示符。
                     * 已取消"回车重复执行上一条命令"的 ENTER 功能，避免
                     * 误按回车把上一条命令（如大段 log tail）重新执行一遍。 */
                    uart_rb_write( ( uint8_t * ) cmdPROMPT_STRING, strlen( cmdPROMPT_STRING ) );
                    xInUserInputMode = pdTRUE;
                }
                else
                {
                    cInputString[ ucInputIndex ] = '\0';

                    do
                    {
                        xReturned = FreeRTOS_CLIProcessCommand( ( char * ) cInputString,
                                                                pcOutputString,
                                                                configCOMMAND_INT_MAX_OUTPUT_SIZE );

                        pcOutputString[ configCOMMAND_INT_MAX_OUTPUT_SIZE - 1 ] = '\0';
                        size_t output_len = strlen( pcOutputString );
                        if( output_len > 0 )
                        {
                            uart_rb_write( ( uint8_t * ) pcOutputString, output_len );
                        }
                        taskYIELD();
                    } while( xReturned != pdFALSE );

                    vAddToHistory( ( char * ) cInputString );

                    ucInputIndex = 0;
                    ucCursorPos = 0;
                    ucHistoryBrowse = 0;
                    memset( ( void * ) cInputString, 0x00, cmdMAX_INPUT_SIZE );

                    uart_rb_write( ( uint8_t * ) pcEndOfOutputMessage,
                                   strlen( pcEndOfOutputMessage ) );
                    uart_rb_write( ( uint8_t * ) cmdPROMPT_STRING,
                                   strlen( cmdPROMPT_STRING ) );

                    xInUserInputMode = pdTRUE;
                }
            }
            else
            {
                if( ( cRxedChar == '\b' ) || ( cRxedChar == cmdASCII_DEL ) )
                {
                    if( ucCursorPos > 0 )
                    {
                        uint8_t i;
                        for( i = ucCursorPos - 1; i < ucInputIndex - 1; i++ )
                        {
                            cInputString[i] = cInputString[i + 1];
                        }
                        ucCursorPos--;
                        ucInputIndex--;
                        cInputString[ ucInputIndex ] = '\0';
                        vRedrawInputLine();
                    }
                }
                else if( ( cRxedChar >= ' ' ) && ( cRxedChar <= '~' ) )
                {
                    if( ucInputIndex < cmdMAX_INPUT_SIZE - 1 )
                    {
                        if( ucCursorPos < ucInputIndex )
                        {
                            uint8_t i;
                            for( i = ucInputIndex; i > ucCursorPos; i-- )
                            {
                                cInputString[i] = cInputString[i - 1];
                            }
                            cInputString[ ucCursorPos ] = cRxedChar;
                            ucInputIndex++;
                            ucCursorPos++;
                            vRedrawInputLine();
                        }
                        else
                        {
                            cInputString[ ucInputIndex ] = cRxedChar;
                            ucInputIndex++;
                            ucCursorPos++;
                            uart_rb_write( ( uint8_t * ) &cRxedChar, sizeof(char) );
                        }
                    }
                }
            }
        }
        
        vRefreshCLIPrompt();
    }
}

void vOutputString( const char * const pcMessage )
{
    uart_rb_write( ( uint8_t * ) pcMessage, strlen( pcMessage ) );
}

void vRefreshCLIPrompt( void )
{
    if( xPromptRefreshNeeded == pdTRUE )
    {
        uart_rb_write( ( uint8_t * ) "\r", sizeof("\r") );
        uart_rb_write( ( uint8_t * ) cmdPROMPT_STRING, strlen( cmdPROMPT_STRING ) );
        if( ucInputIndex > 0 )
        {
            uart_rb_write( ( uint8_t * ) cInputString, ucInputIndex );
        }
        uart_rb_write( ( uint8_t * ) "\x1B[K", sizeof("\x1B[K") );
        uint8_t moveBack = ucInputIndex - ucCursorPos;
        while( moveBack > 0 )
        {
            uart_rb_write( ( uint8_t * ) "\b", sizeof("\b") );
            moveBack--;
        }
        xPromptRefreshNeeded = pdFALSE;
        xPromptDisplayed = pdTRUE;
    }
}

void vSetPromptRefreshNeeded( void )
{
    xPromptRefreshNeeded = pdTRUE;
}
