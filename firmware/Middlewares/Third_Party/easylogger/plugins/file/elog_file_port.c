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
 * Function:  Portable interface for EasyLogger's file log plugin with LittleFS.
 * Created on: 2026-06-27
 */

#include "elog_file.h"
#include "elog_file_cfg.h"
#include "lfs_port.h"
#include "lfs.h"
#include "cmsis_os.h"
#include <string.h>
#include <stdio.h>

#define ELOG_FILE_MAX_OPEN  2

static osMutexId_t fileMutex = NULL;
static volatile uint32_t lock_acquire_count = 0;
static volatile uint32_t lock_release_count = 0;
static volatile uint32_t lock_recursion_depth = 0;

typedef struct {
    lfs_file_t file;
    int32_t mode;
    bool inUse;
} ElogFileHandle;

static ElogFileHandle fileHandles[ELOG_FILE_MAX_OPEN] = {0};

#define ELOG_FILE_MODE_READ    (1 << 0)
#define ELOG_FILE_MODE_WRITE   (1 << 1)
#define ELOG_FILE_MODE_APPEND  (1 << 2)

static inline lfs_t *get_lfs(void)
{
    return lfs_port_get_lfs();
}

static inline struct lfs_config *get_lfs_config(void)
{
    return lfs_port_get_lfs_config();
}

ElogErrCode elog_file_port_init(void)
{
    const osMutexAttr_t mutexAttr = {
        .name = "elog_file_mutex",
        .attr_bits = osMutexRecursive,
        .cb_mem = NULL,
        .cb_size = 0U,
    };

    if (get_lfs() == NULL || get_lfs_config() == NULL) {
        return (ElogErrCode)osErrorParameter;
    }

    fileMutex = osMutexNew(&mutexAttr);
    if (fileMutex == NULL) {
        return (ElogErrCode)osErrorResource;
    }

    return ELOG_NO_ERR;
}

void elog_file_port_lock(void)
{
    if (fileMutex != NULL) {
        osMutexAcquire(fileMutex, osWaitForever);
        lock_acquire_count++;
        lock_recursion_depth++;
    }
}

void elog_file_port_unlock(void)
{
    if (fileMutex != NULL) {
        lock_release_count++;
        lock_recursion_depth--;
        osMutexRelease(fileMutex);
    }
}

void elog_file_port_deinit(void)
{
    int32_t i;

    for (i = 0; i < ELOG_FILE_MAX_OPEN; i++) {
        if (fileHandles[i].inUse) {
            lfs_file_close(get_lfs(), &fileHandles[i].file);
            fileHandles[i].inUse = false;
        }
    }

    if (fileMutex != NULL) {
        osMutexDelete(fileMutex);
        fileMutex = NULL;
    }
    lfs_unmount(get_lfs());
}

void elog_file_flush_all(void)
{
    int32_t i;
    elog_file_port_lock();
    for (i = 0; i < ELOG_FILE_MAX_OPEN; i++) {
        if (fileHandles[i].inUse && (fileHandles[i].mode & ELOG_FILE_MODE_WRITE)) {
            lfs_file_sync(get_lfs(), &fileHandles[i].file);
        }
    }
    elog_file_port_unlock();
}

void *elog_file_port_fopen(const char *path, const char *mode)
{
    ElogFileHandle *fh = NULL;
    int32_t flags = 0;
    int32_t err;
    int32_t i;

    elog_file_port_lock();

    for (i = 0; i < ELOG_FILE_MAX_OPEN; i++) {
        if (!fileHandles[i].inUse) {
            fh = &fileHandles[i];
            break;
        }
    }

    if (fh == NULL) {
        elog_file_port_unlock();
        return NULL;
    }

    memset(fh, 0, sizeof(ElogFileHandle));

    if (strstr(mode, "r") != NULL) {
        flags |= LFS_O_RDONLY;
        fh->mode |= ELOG_FILE_MODE_READ;
    }
    if (strstr(mode, "w") != NULL) {
        flags |= LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC;
        fh->mode |= ELOG_FILE_MODE_WRITE;
    }
    if (strstr(mode, "a") != NULL) {
        flags |= LFS_O_WRONLY | LFS_O_CREAT | LFS_O_APPEND;
        fh->mode |= ELOG_FILE_MODE_WRITE | ELOG_FILE_MODE_APPEND;
    }
    if (strstr(mode, "+") != NULL) {
        flags |= LFS_O_RDWR;
        fh->mode |= ELOG_FILE_MODE_READ | ELOG_FILE_MODE_WRITE;
    }

    err = lfs_file_open(get_lfs(), &fh->file, path, flags);
    if (err != LFS_ERR_OK) {
        elog_file_port_unlock();
        return NULL;
    }

    fh->inUse = true;
    elog_file_port_unlock();
    return fh;
}

void elog_file_port_fclose(void *fp)
{
    ElogFileHandle *fh = (ElogFileHandle *)fp;
    if (fh != NULL && fh->inUse) {
        elog_file_port_lock();
        lfs_file_close(get_lfs(), &fh->file);
        fh->inUse = false;
        elog_file_port_unlock();
    }
}

size_t elog_file_port_fwrite(const void *ptr, size_t size, size_t nmemb, void *fp)
{
    ElogFileHandle *fh = (ElogFileHandle *)fp;
    lfs_ssize_t result;

    if (fh == NULL || ptr == NULL || !(fh->mode & ELOG_FILE_MODE_WRITE)) {
        return 0;
    }

    elog_file_port_lock();
    result = lfs_file_write(get_lfs(), &fh->file, ptr, size * nmemb);
    if (result >= 0) {
#if ELOG_FILE_SYNC_ON_WRITE
        lfs_file_sync(get_lfs(), &fh->file);
#endif
    }
    elog_file_port_unlock();

    return (result < 0) ? 0 : (size_t)(result / size);
}

size_t elog_file_port_fread(void *ptr, size_t size, size_t nmemb, void *fp)
{
    ElogFileHandle *fh = (ElogFileHandle *)fp;
    lfs_ssize_t result;

    if (fh == NULL || ptr == NULL || !(fh->mode & ELOG_FILE_MODE_READ)) {
        return 0;
    }

    elog_file_port_lock();
    result = lfs_file_read(get_lfs(), &fh->file, ptr, size * nmemb);
    elog_file_port_unlock();

    return (result < 0) ? 0 : (size_t)(result / size);
}

int32_t elog_file_port_fseek(void *fp, int32_t offset, int32_t whence)
{
    ElogFileHandle *fh = (ElogFileHandle *)fp;
    lfs_soff_t result;
    int32_t lfsWhence;

    if (fh == NULL) {
        return -1;
    }

    switch (whence) {
        case SEEK_SET:
            lfsWhence = LFS_SEEK_SET;
            break;
        case SEEK_CUR:
            lfsWhence = LFS_SEEK_CUR;
            break;
        case SEEK_END:
            lfsWhence = LFS_SEEK_END;
            break;
        default:
            return -1;
    }

    elog_file_port_lock();
    result = lfs_file_seek(get_lfs(), &fh->file, offset, lfsWhence);
    elog_file_port_unlock();

    return (result < 0) ? -1 : 0;
}

int32_t elog_file_port_ftell(void *fp)
{
    ElogFileHandle *fh = (ElogFileHandle *)fp;
    lfs_soff_t result;

    if (fh == NULL) {
        return -1;
    }

    elog_file_port_lock();
    result = lfs_file_tell(get_lfs(), &fh->file);
    elog_file_port_unlock();

    return (result < 0) ? -1 : (int32_t)result;
}

int32_t elog_file_port_remove(const char *path)
{
    int32_t err;

    if (path == NULL) {
        return -1;
    }

    elog_file_port_lock();
    err = lfs_remove(get_lfs(), path);
    elog_file_port_unlock();

    return (err != LFS_ERR_OK) ? -1 : 0;
}

int32_t elog_file_port_rename(const char *oldpath, const char *newpath)
{
    int32_t err;

    if (oldpath == NULL || newpath == NULL) {
        return -1;
    }

    elog_file_port_lock();
    err = lfs_rename(get_lfs(), oldpath, newpath);
    elog_file_port_unlock();

    return (err != LFS_ERR_OK) ? -1 : 0;
}

void elog_file_flush_all_isr(void)
{
    int32_t i;

    for (i = 0; i < ELOG_FILE_MAX_OPEN; i++) {
        if (fileHandles[i].inUse && (fileHandles[i].mode & ELOG_FILE_MODE_WRITE)) {
            lfs_file_sync(get_lfs(), &fileHandles[i].file);
        }
    }
}

void elog_file_port_get_lock_stats(uint32_t *acquire_count, uint32_t *release_count, uint32_t *recursion_depth)
{
    if (acquire_count != NULL) {
        *acquire_count = lock_acquire_count;
    }
    if (release_count != NULL) {
        *release_count = lock_release_count;
    }
    if (recursion_depth != NULL) {
        *recursion_depth = lock_recursion_depth;
    }
}

lfs_t *elog_file_port_get_lfs(void)
{
    return get_lfs();
}

struct lfs_config *elog_file_port_get_lfs_config(void)
{
    return get_lfs_config();
}
