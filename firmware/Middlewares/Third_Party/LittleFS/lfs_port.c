/*
 * lfs_port.c
 */

#include "lfs_port.h"
#include <string.h>

static lfs_port_context_t s_context = {
    .flash = NULL,
    .mounted = 0,
};

#define LFS_FLASH_START_ADDR    (0x0)
#define LFS_READ_SIZE           (256)
#define LFS_CACHE_SIZE          (256)

#define LFS_LOOKAHEAD_SIZE_BASE (64)
#define LFS_LOOKAHEAD_SIZE_MAX  (256)

int lfs_port_register(sfud_flash *flash)
{
    if (flash == NULL) {
        return -1;
    }
    s_context.flash = flash;
    return 0;
}

const lfs_port_context_t *lfs_port_get_context(void)
{
    return &s_context;
}

lfs_t *lfs_port_get_lfs(void)
{
    return &s_context.lfs;
}

struct lfs_config *lfs_port_get_lfs_config(void)
{
    return &s_context.lfs_cfg;
}

sfud_flash *lfs_port_get_flash(void)
{
    return s_context.flash;
}

static uint32_t get_block_size(void)
{
    if (s_context.flash == NULL) {
        return 4 * 1024;
    }
    return s_context.flash->chip.erase_gran;
}

static uint32_t get_block_count(void)
{
    if (s_context.flash == NULL) {
        return 0;
    }
    uint32_t capacity = s_context.flash->chip.capacity;
    uint32_t block_size = get_block_size();
    if (block_size == 0) {
        return 0;
    }
    return capacity / block_size;
}

static uint32_t get_prog_size(void)
{
    if (s_context.flash == NULL) {
        return 256;
    }
    uint16_t write_mode = s_context.flash->chip.write_mode;
    if (write_mode & SFUD_WM_PAGE_256B) {
        return 256;
    } else if (write_mode & SFUD_WM_BYTE) {
        return 1;
    }
    return 256;
}

int lfs_spi_flash_read(const struct lfs_config *cfg, lfs_block_t block, lfs_off_t off, void *buffer, lfs_size_t size)
{
    (void)cfg;
    if (s_context.flash == NULL) {
        return LFS_ERR_IO;
    }
    
    uint32_t block_size = get_block_size();
    uint32_t addr = LFS_FLASH_START_ADDR + (uint32_t)block * block_size + (uint32_t)off;
    sfud_err result = sfud_read(s_context.flash, addr, size, buffer);
    
    return result == SFUD_SUCCESS ? LFS_ERR_OK : LFS_ERR_IO;
}

int lfs_spi_flash_prog(const struct lfs_config *cfg, lfs_block_t block, lfs_off_t off, const void *buffer, lfs_size_t size)
{
    (void)cfg;
    if (s_context.flash == NULL) {
        return LFS_ERR_IO;
    }
    
    uint32_t block_size = get_block_size();
    uint32_t addr = LFS_FLASH_START_ADDR + (uint32_t)block * block_size + (uint32_t)off;
    sfud_err result = sfud_write(s_context.flash, addr, size, buffer);
    
    return result == SFUD_SUCCESS ? LFS_ERR_OK : LFS_ERR_IO;
}

int lfs_spi_flash_erase(const struct lfs_config *cfg, lfs_block_t block)
{
    (void)cfg;
    if (s_context.flash == NULL) {
        return LFS_ERR_IO;
    }
    
    uint32_t block_size = get_block_size();
    uint32_t addr = LFS_FLASH_START_ADDR + (uint32_t)block * block_size;
    sfud_err result = sfud_erase(s_context.flash, addr, block_size);
    
    return result == SFUD_SUCCESS ? LFS_ERR_OK : LFS_ERR_IO;
}

int lfs_spi_flash_sync(const struct lfs_config *cfg)
{
    (void)cfg;
    return LFS_ERR_OK;
}

int lfs_spi_flash_init(struct lfs_config *cfg)
{
    if (s_context.flash == NULL) {
        return LFS_ERR_IO;
    }
    
    uint32_t block_size = get_block_size();
    uint32_t block_count = get_block_count();
    uint32_t prog_size = get_prog_size();
    
    memset(cfg, 0, sizeof(struct lfs_config));
    
    cfg->read = lfs_spi_flash_read;
    cfg->prog = lfs_spi_flash_prog;
    cfg->erase = lfs_spi_flash_erase;
    cfg->sync = lfs_spi_flash_sync;
    
    cfg->read_size = LFS_READ_SIZE;
    cfg->prog_size = prog_size;
    cfg->block_size = block_size;
    cfg->block_count = block_count;
    cfg->cache_size = LFS_CACHE_SIZE;
    
    if (block_count <= 128) {
        cfg->lookahead_size = LFS_LOOKAHEAD_SIZE_BASE;
    } else if (block_count <= 512) {
        cfg->lookahead_size = 128;
    } else {
        cfg->lookahead_size = LFS_LOOKAHEAD_SIZE_MAX;
    }
    
    cfg->block_cycles = 100;
    cfg->context = NULL;
    
    return LFS_ERR_OK;
}
