/*
 * lfs_port.h
 *
 *  Created on: Jun 25, 2026
 *      Author: wangqi
 */

#ifndef _LFS_PORT_H_
#define _LFS_PORT_H_

#include"lfs.h"

/* LittleFS 底层读操作 */
int lfs_spi_flash_read(const struct lfs_config *cfg, lfs_block_t block, lfs_off_t off, void *buffer, lfs_size_t size);

/* LittleFS 底层写操作（写前需擦除）*/
int lfs_spi_flash_prog(const struct lfs_config *cfg, lfs_block_t block, lfs_off_t off, const void *buffer, lfs_size_t size);

/* LittleFS 底层擦除一个块 */
int lfs_spi_flash_erase(const struct lfs_config *cfg, lfs_block_t block);

/* LittleFS 底层同步操作（SPI Flash 无需）*/
int lfs_spi_flash_sync(const struct lfs_config *cfg);
int lfs_spi_flash_init(struct lfs_config *cfg);


#endif /* _LFS_PORT_H_ */
