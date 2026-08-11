/*
 * This file is part of the EasyLogger Library.
 *
 * Copyright (c) 2015-2016, Armink, <armink.ztl@gmail.com>
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
 * Created on: 2015-07-30
 */

#ifndef _ELOG_CFG_H_
#define _ELOG_CFG_H_
/*---------------------------------------------------------------------------*/
/* enable log output. */
#define ELOG_OUTPUT_ENABLE
/* setting static output log level. range: from ELOG_LVL_ASSERT to ELOG_LVL_VERBOSE */
#define ELOG_OUTPUT_LVL                          ELOG_LVL_VERBOSE
/* enable assert check */
#define ELOG_ASSERT_ENABLE
/* buffer size for every line's log */
/* 系统日志单行通常 < 200 字符，512 足够；1024 -> 512 节省 512B RAM（当前 RAM 紧张） */
#define ELOG_LINE_BUF_SIZE                       512
/* output line number max length */
#define ELOG_LINE_NUM_MAX_LEN                    5
/* output filter's tag max length */
#define ELOG_FILTER_TAG_MAX_LEN                  30
/* output filter's keyword max length */
#define ELOG_FILTER_KW_MAX_LEN                   16
/* output filter's tag level max num */
#define ELOG_FILTER_TAG_LVL_MAX_NUM              5
/* output newline sign */
#define ELOG_NEWLINE_SIGN                        "\r\n"
/*---------------------------------------------------------------------------*/
/* enable log color */
#define ELOG_COLOR_ENABLE
/* change the some level logs to not default color if you want */
#define ELOG_COLOR_ASSERT                        (F_MAGENTA B_NULL S_NORMAL)
#define ELOG_COLOR_ERROR                         (F_RED B_NULL S_NORMAL)
#define ELOG_COLOR_WARN                          (F_YELLOW B_NULL S_NORMAL)
#define ELOG_COLOR_INFO                          (F_CYAN B_NULL S_NORMAL)
#define ELOG_COLOR_DEBUG                         (F_GREEN B_NULL S_NORMAL)
#define ELOG_COLOR_VERBOSE                       (F_BLUE B_NULL S_NORMAL)
/*---------------------------------------------------------------------------*/
/* enable log fmt */
/* comment it if you don't want to output them at all */
#define ELOG_FMT_USING_FUNC
#define ELOG_FMT_USING_DIR
#define ELOG_FMT_USING_LINE
/*---------------------------------------------------------------------------*/
/* enable asynchronous output mode */
#define ELOG_ASYNC_OUTPUT_ENABLE
/* the highest output level for async mode, other level will sync output */
#define ELOG_ASYNC_OUTPUT_LVL                    ELOG_LVL_VERBOSE
/* buffer size for asynchronous output mode */
/* 10KB -> 4KB（512x8）：环形缓冲按实际日志吞吐评估，缩小以释放 RAM 给 LwIP */
#define ELOG_ASYNC_OUTPUT_BUF_SIZE               (ELOG_LINE_BUF_SIZE * 8)
/* each asynchronous output's log which must end with newline sign */
#define ELOG_ASYNC_LINE_OUTPUT
/* asynchronous output mode using POSIX pthread implementation */
/* FreeRTOS: Use our custom pthread port instead of system pthread */
#define ELOG_ASYNC_OUTPUT_USING_PTHREAD
#define ELOG_USING_FREERTOS_PTHREAD
/*---------------------------------------------------------------------------*/
/* enable buffered output mode */
/* ============================================================
 * 注意：缓冲输出模式已通过条件编译禁用（勿直接在源码中改回）。
 * 原因：buf 模式与异步模式（ELOG_ASYNC_OUTPUT_ENABLE）互斥，
 * elog.c 中启用 async 时 buf 模式的 10KB 静态缓冲从未使用却仍占 RAM。
 * 禁用后 elog_buf.c 由 #ifdef ELOG_BUF_OUTPUT_ENABLE 整体裁掉，
 * 释放 10KB RAM（当前 RAM 紧张，需为 LwIP 腾空间）。
 * 如需恢复，取消下面注释并同时关闭 ELOG_ASYNC_OUTPUT_ENABLE。
 * ============================================================ */
/* #define ELOG_BUF_OUTPUT_ENABLE */
/* buffer size for buffered output mode */
#define ELOG_BUF_OUTPUT_BUF_SIZE                 (ELOG_LINE_BUF_SIZE * 8)
/*---------------------------------------------------------------------------*/
/* enable file log plugin */
#define ELOG_FILE_ENABLE
/* flush cache when write log to file */
#define ELOG_FILE_FLUSH_CACHE_ENABLE

#endif /* _ELOG_CFG_H_ */
