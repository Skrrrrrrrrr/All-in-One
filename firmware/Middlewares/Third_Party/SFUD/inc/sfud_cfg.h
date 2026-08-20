/*
 * This file is part of the Serial Flash Universal Driver Library.
 *
 * Copyright (c) 2016-2018, Armink, <armink.ztl@gmail.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * 'Software'), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED 'AS IS', WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 * Function: It is the configure head file for this library.
 * Created on: 2016-04-23
 */

#ifndef _SFUD_CFG_H_
#define _SFUD_CFG_H_

#define SFUD_DEBUG_MODE

/* SFDP probing DISABLED.
 * This BSP uses a FIXED Winbond W25Q128BV (JEDEC ID 0xEF/0x40/0x18) which is
 * already present in SFUD's static flash_chip_table (see sfud_flash_def.h).
 * SFDP probing is therefore redundant, and right after the flash reset it
 * intermittently returns a bad signature -> non-deterministic boot
 * ("Check SFDP signature error" + fallback to the static table, at random).
 * Using the static table makes sfud_init() deterministic and removes the
 * error spam. The static entry (16MB, 256B page, 4KB erase 0x20) is identical
 * to what SFDP would report, so flash read/erase/write behavior is unchanged. */
// #define SFUD_USING_SFDP

// #define SFUD_USING_FAST_READ

#define SFUD_USING_FLASH_INFO_TABLE

//#define SFUD_USING_SPI_DMA

enum {
//    SFUD_XXXX_DEVICE_INDEX = 0,
//	SFUD_W25Q32BV_DEVICE_INDEX,
	SFUD_W25Q128BV_DEVICE_INDEX
};

#define SFUD_FLASH_DEVICE_TABLE                                                \
{                                                                              \
    [SFUD_W25Q128BV_DEVICE_INDEX] = {.name = "W25Q128BV", .spi.name = "SPI1"}, \
}

//#define SFUD_USING_QSPI

#endif /* _SFUD_CFG_H_ */
