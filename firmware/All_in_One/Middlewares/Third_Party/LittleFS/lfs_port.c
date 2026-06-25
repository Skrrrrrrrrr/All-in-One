/*
 * lfs_port.c
 *
 *  Created on: Jun 25, 2026
 *      Author: wangqi
 */


#include "lfs_port.h"
#include "sfud.h"

/* SFUD 设备句柄 */
static sfud_flash *h_sfud_flash = NULL;


/* LittleFS 底层读操作 */
int lfs_spi_flash_read(const struct lfs_config *cfg, lfs_block_t block, lfs_off_t off, void *buffer, lfs_size_t size)
{
    LFS_ASSERT(off % cfg->read_size == 0);
    LFS_ASSERT(size % cfg->read_size == 0);
    LFS_ASSERT(block < cfg->block_count);

    sfud_err r = sfud_read(h_sfud_flash, block * cfg->block_size + off, size, (uint8_t *)buffer);
    //printf("lfs read block=%lu off=%lu size=%lu r = %d\r\n", block, off, size, r);
    return (r == SFUD_SUCCESS) ? LFS_ERR_OK : LFS_ERR_IO;
}

/* LittleFS 底层写操作（写前需擦除）*/
int lfs_spi_flash_prog(const struct lfs_config *cfg, lfs_block_t block, lfs_off_t off, const void *buffer,
                       lfs_size_t size)
{
    LFS_ASSERT(off % cfg->prog_size == 0);
    LFS_ASSERT(size % cfg->prog_size == 0);
    LFS_ASSERT(block < cfg->block_count);

    sfud_err r = sfud_write(h_sfud_flash, block * cfg->block_size + off, size, (const uint8_t *)buffer);
    //printf("lfs prog block=%lu off=%lu size=%lu r = %d\r\n", block, off, size, r);
    return (r == SFUD_SUCCESS) ? LFS_ERR_OK : LFS_ERR_IO;
}

/* LittleFS 底层擦除一个块 */
int lfs_spi_flash_erase(const struct lfs_config *cfg, lfs_block_t block)
{
    LFS_ASSERT(block < cfg->block_count);

    sfud_err r = sfud_erase(h_sfud_flash, block * cfg->block_size, cfg->block_size);
    //printf("lfs erase block=%lu r = %d\r\n", block, r);
    return (r == SFUD_SUCCESS) ? LFS_ERR_OK : LFS_ERR_IO;
}

/* LittleFS 底层同步操作（SPI Flash 无需）*/
int lfs_spi_flash_sync(const struct lfs_config *cfg)
{
    return LFS_ERR_OK;
}

int lfs_spi_flash_init(struct lfs_config *cfg)
{
    if (sfud_init() != SFUD_SUCCESS)
        return LFS_ERR_IO;

    h_sfud_flash = sfud_get_device(SFUD_W25_DEVICE_INDEX);
    if (!h_sfud_flash || sfud_device_init(h_sfud_flash) != SFUD_SUCCESS)
    {
        return LFS_ERR_IO;
    }

    cfg->read = lfs_spi_flash_read;
    cfg->prog = lfs_spi_flash_prog;
    cfg->erase = lfs_spi_flash_erase;
    cfg->sync = lfs_spi_flash_sync;

    cfg->read_size = 16;
    cfg->prog_size = 256;
    cfg->block_size = 4096;
    cfg->block_count = 4096;
    cfg->block_cycles = 500;
    cfg->cache_size = 256; // 必须能被 prog_size 整除

    cfg->lookahead_size = 4096 / 8;

    cfg->compact_thresh = (lfs_size_t)-1;

    cfg->inline_max = cfg->cache_size;

    return LFS_ERR_OK;
}

