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
 * Uses existing USART1 HAL driver and uart_ringbuf for DMA output
 */

#include "FreeRTOS.h"
#include "queue.h"
#include "semphr.h"

#include "stm32f4xx_hal.h"
#include "usart.h"
#include "uart_ringbuf.h"

#include "serial.h"

#define serINVALID_QUEUE        ( ( QueueHandle_t ) 0 )
#define serNO_BLOCK             ( ( TickType_t ) 0 )
#define serTX_BLOCK_TIME        ( 40 / portTICK_PERIOD_MS )

static QueueHandle_t xRxedChars;
static QueueHandle_t xCharsForTx;
static volatile uint32_t ulRxQueueOverflowCount = 0;

extern volatile BaseType_t xInUserInputMode;
extern volatile uint8_t ucInputIndex;
extern volatile char cInputString[];

void vSetPromptRefreshNeeded( void );

xComPortHandle xSerialPortInitMinimal( unsigned long ulWantedBaud, unsigned portBASE_TYPE uxQueueLength )
{
    xComPortHandle xReturn;

    xRxedChars = xQueueCreate( uxQueueLength, sizeof( signed char ) );
    xCharsForTx = xQueueCreate( uxQueueLength + 1, sizeof( signed char ) );

    if( ( xRxedChars != serINVALID_QUEUE ) && ( xCharsForTx != serINVALID_QUEUE ) )
    {
        xReturn = ( xComPortHandle ) 1;
    }
    else
    {
        xReturn = ( xComPortHandle ) 0;
    }

    return xReturn;
}

signed portBASE_TYPE xSerialGetChar( xComPortHandle pxPort, signed char *pcRxedChar, TickType_t xBlockTime )
{
    ( void ) pxPort;

    if( xQueueReceive( xRxedChars, pcRxedChar, xBlockTime ) )
    {
        return pdTRUE;
    }
    else
    {
        return pdFALSE;
    }
}

void vSerialPutString( xComPortHandle pxPort, const signed char * const pcString, unsigned short usStringLength )
{
    ( void ) usStringLength;
    ( void ) pxPort;

    signed char *pxNext = ( signed char * ) pcString;
    while( *pxNext )
    {
        xSerialPutChar( pxPort, *pxNext, serNO_BLOCK );
        pxNext++;
    }
}

signed portBASE_TYPE xSerialPutChar( xComPortHandle pxPort, signed char cOutChar, TickType_t xBlockTime )
{
    signed portBASE_TYPE xReturn;

    ( void ) pxPort;

    uart_rb_write( ( uint8_t * ) &cOutChar, sizeof(char) );
    xReturn = pdPASS;

    return xReturn;
}

void vSerialClose( xComPortHandle xPort )
{
    ( void ) xPort;
}

void serial_rx_handler( uint8_t cChar )
{
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;

    if( xQueueSendFromISR( xRxedChars, &cChar, &xHigherPriorityTaskWoken ) != pdPASS )
    {
        ulRxQueueOverflowCount++;
    }

    portEND_SWITCHING_ISR( xHigherPriorityTaskWoken );
}

uint32_t ulSerialGetRxOverflowCount( void )
{
    return ulRxQueueOverflowCount;
}

BaseType_t serial_get_input_mode( void )
{
    return xInUserInputMode;
}

uint8_t serial_get_input_len( void )
{
    return ucInputIndex;
}

void serial_get_input_string( char *buf, uint8_t len )
{
    uint8_t i;
    for( i = 0; i < len && i < ucInputIndex; i++ )
    {
        buf[i] = cInputString[i];
    }
    buf[i] = '\0';
}
