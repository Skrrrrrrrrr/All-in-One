/*
 * lfs_port.h
 */

#ifndef _LFS_PORT_H_
#define _LFS_PORT_H_

#include "lfs.h"
#include "sfud.h"

typedef struct {
    sfud_flash *flash;
    struct lfs_config lfs_cfg;
    lfs_t lfs;
    uint8_t mounted;
} lfs_port_context_t;

int lfs_port_register(sfud_flash *flash);
const lfs_port_context_t *lfs_port_get_context(void);
lfs_t *lfs_port_get_lfs(void);
struct lfs_config *lfs_port_get_lfs_config(void);
sfud_flash *lfs_port_get_flash(void);

int lfs_spi_flash_read(const struct lfs_config *cfg, lfs_block_t block, lfs_off_t off, void *buffer, lfs_size_t size);
int lfs_spi_flash_prog(const struct lfs_config *cfg, lfs_block_t block, lfs_off_t off, const void *buffer, lfs_size_t size);
int lfs_spi_flash_erase(const struct lfs_config *cfg, lfs_block_t block);
int lfs_spi_flash_sync(const struct lfs_config *cfg);
int lfs_spi_flash_init(struct lfs_config *cfg);

#endif /* _LFS_PORT_H_ */
