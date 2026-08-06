/*
 * This file is part of the EasyLogger Library.
 *
 * Copyright (c) 2015-2019, Qintl, <qintl_linux@163.com>
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
 * Function:  It is an head file for file log plugin. You can see all be called functions.
 * Created on: 2019-01-05
 */

#ifndef __ELOG_FILE__H__
#define __ELOG_FILE__H__

#include <stdio.h>
#include <stdint.h>
#include <elog.h>
#include <elog_file_cfg.h>
#include <lfs.h>

#ifdef __cplusplus
extern "C" {
#endif

/* EasyLogger file log plugin's software version number */
#define ELOG_FILE_SW_VERSION                "V1.0.0"
#ifdef linux
#define likely(x) __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)
#else
#define likely(x)   (x)
#define unlikely(x) (x)
#endif

typedef struct {
    char *name;              /* file name */
    size_t max_size;         /* file max size */
    int max_rotate;          /* max rotate file count */
} ElogFileCfg;

/* elog_file.c */
ElogErrCode elog_file_init(void);
void elog_file_write(const char *log, size_t size);
void elog_file_config(ElogFileCfg *cfg);
void elog_file_deinit(void);

/* elog_file_port.c */
ElogErrCode elog_file_port_init(void);
void elog_file_port_lock(void);
void elog_file_port_unlock(void);
void elog_file_port_deinit(void);
void elog_file_flush_all(void);
void elog_file_flush_all_isr(void);

/* File operation abstraction layer for portability */
void *elog_file_port_fopen(const char *path, const char *mode);
void elog_file_port_fclose(void *fp);
size_t elog_file_port_fwrite(const void *ptr, size_t size, size_t nmemb, void *fp);
size_t elog_file_port_fread(void *ptr, size_t size, size_t nmemb, void *fp);
int32_t elog_file_port_fseek(void *fp, int32_t offset, int32_t whence);
int32_t elog_file_port_ftell(void *fp);
int32_t elog_file_port_remove(const char *path);
int32_t elog_file_port_rename(const char *oldpath, const char *newpath);
void elog_file_port_get_lock_stats(uint32_t *acquire_count, uint32_t *release_count, uint32_t *recursion_depth);

lfs_t *elog_file_port_get_lfs(void);
struct lfs_config *elog_file_port_get_lfs_config(void);

#ifdef __cplusplus
}
#endif

#endif
