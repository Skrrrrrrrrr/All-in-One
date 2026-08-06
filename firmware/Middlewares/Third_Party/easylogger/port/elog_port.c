/*
 * This file is part of the EasyLogger Library.
 *
 * Copyright (c) 2015, Armink, <armink.ztl@gmail.com>
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
 * Function: Portable interface for each platform.
 * Created on: 2015-04-28
 */

#include <elog.h>
#include "FreeRTOS.h"
#include "cmsis_os.h"

#ifdef ELOG_FILE_ENABLE
#include "elog_file.h"
#endif

typedef void (*elog_output_cb_t)(const uint8_t *data, size_t size);
typedef void (*elog_prompt_refresh_cb_t)(void);
typedef BaseType_t (*elog_input_mode_check_cb_t)(void);
typedef uint8_t (*elog_input_len_get_cb_t)(void);
typedef void (*elog_input_get_cb_t)(char *buf, uint8_t len);

typedef struct {
    elog_output_cb_t output_cb;
    elog_prompt_refresh_cb_t prompt_refresh_cb;
    elog_input_mode_check_cb_t input_mode_check_cb;
    elog_input_len_get_cb_t input_len_get_cb;
    elog_input_get_cb_t input_get_cb;
} elog_port_context_t;

static elog_port_context_t s_context = {
    .output_cb = NULL,
    .prompt_refresh_cb = NULL,
    .input_mode_check_cb = NULL,
    .input_len_get_cb = NULL,
    .input_get_cb = NULL,
};

int elog_port_register(elog_output_cb_t output_cb,
                       elog_prompt_refresh_cb_t prompt_refresh_cb,
                       elog_input_mode_check_cb_t input_mode_check_cb,
                       elog_input_len_get_cb_t input_len_get_cb,
                       elog_input_get_cb_t input_get_cb)
{
    if (output_cb == NULL) {
        return -1;
    }
    s_context.output_cb = output_cb;
    s_context.prompt_refresh_cb = prompt_refresh_cb;
    s_context.input_mode_check_cb = input_mode_check_cb;
    s_context.input_len_get_cb = input_len_get_cb;
    s_context.input_get_cb = input_get_cb;
    return 0;
}

const elog_port_context_t *elog_port_get_context(void)
{
    return &s_context;
}

ElogErrCode elog_port_init(void) {
    ElogErrCode result = ELOG_NO_ERR;

#ifdef ELOG_FILE_ENABLE
    result = elog_file_init();
#endif

    return result;
}

void elog_port_deinit(void) {
#ifdef ELOG_FILE_ENABLE
    elog_file_deinit();
#endif
}

void elog_port_output(const char *log, size_t size) {
    BaseType_t xInInputMode = pdFALSE;

    if (s_context.input_mode_check_cb != NULL) {
        xInInputMode = s_context.input_mode_check_cb();
    }

    if (xInInputMode == pdTRUE && s_context.output_cb != NULL) {
        s_context.output_cb((uint8_t*)"\r", 1);
        uint8_t inputLen = 0;
        if (s_context.input_len_get_cb != NULL) {
            inputLen = s_context.input_len_get_cb();
        }
        uint8_t i;
        for (i = 0; i < inputLen + 2; i++) {
            s_context.output_cb((uint8_t*)" ", 1);
        }
        s_context.output_cb((uint8_t*)"\r", 1);
    }

    if (s_context.output_cb != NULL) {
        s_context.output_cb((uint8_t*)log, size);
    }

    if (xInInputMode == pdTRUE && s_context.prompt_refresh_cb != NULL) {
        s_context.prompt_refresh_cb();
    }

#ifdef ELOG_FILE_ENABLE
    elog_file_write(log, size);
#endif
}

void elog_port_output_lock(void) {
}

void elog_port_output_unlock(void) {
}

const char *elog_port_get_time(void) {
    static char time_str[20] = "00:00:00";
    return time_str;
}

const char *elog_port_get_p_info(void) {
    return "STM32";
}

const char *elog_port_get_t_info(void) {
    return "main";
}
