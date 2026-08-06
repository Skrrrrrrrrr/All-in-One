#ifndef __ELOG_PTHREAD_PORT_H__
#define __ELOG_PTHREAD_PORT_H__

#include "cmsis_os.h"
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef osThreadId_t pthread_t;
typedef void* pthread_attr_t;
typedef void* pthread_mutexattr_t;
typedef osSemaphoreId_t sem_t;

typedef void* (*pthread_func_t)(void*);

#define SCHED_RR        1
#define SCHED_FIFO      2

#define PTHREAD_CREATE_DETACHED    0x01
#define PTHREAD_CREATE_JOINABLE    0x00

#define PTHREAD_MUTEX_INITIALIZER  NULL

struct sched_param {
    int32_t sched_priority;
};

#ifndef ELOG_ASYNC_OUTPUT_PTHREAD_STACK_SIZE
#define ELOG_ASYNC_OUTPUT_PTHREAD_STACK_SIZE     (8192)
#endif

#ifndef ELOG_ASYNC_OUTPUT_PTHREAD_PRIORITY
#define ELOG_ASYNC_OUTPUT_PTHREAD_PRIORITY       osPriorityNormal
#endif

#ifdef ELOG_ASYNC_LINE_OUTPUT
#ifndef ELOG_ASYNC_POLL_GET_LOG_BUF_SIZE
#define ELOG_ASYNC_POLL_GET_LOG_BUF_SIZE         (ELOG_LINE_BUF_SIZE - 4)
#endif
#else
#ifndef ELOG_ASYNC_POLL_GET_LOG_BUF_SIZE
#define ELOG_ASYNC_POLL_GET_LOG_BUF_SIZE         (ELOG_ASYNC_OUTPUT_BUF_SIZE - 4)
#endif
#endif

static inline int32_t pthread_attr_init(pthread_attr_t *attr) {
    if (attr) {
        memset(attr, 0, sizeof(pthread_attr_t));
    }
    return 0;
}

static inline int32_t pthread_attr_destroy(pthread_attr_t *attr) {
    (void)attr;
    return 0;
}

static inline int32_t pthread_attr_setstacksize(pthread_attr_t *attr, size_t stack_size) {
    (void)attr;
    (void)stack_size;
    return 0;
}

static inline int32_t pthread_attr_setdetachstate(pthread_attr_t *attr, int32_t detachstate) {
    (void)attr;
    (void)detachstate;
    return 0;
}

static inline int32_t pthread_attr_setschedpolicy(pthread_attr_t *attr, int32_t policy) {
    (void)attr;
    (void)policy;
    return 0;
}

static inline int32_t pthread_attr_setschedparam(pthread_attr_t *attr, struct sched_param *param) {
    (void)attr;
    (void)param;
    return 0;
}

static inline int32_t sched_get_priority_max(int32_t policy) {
    (void)policy;
    return (int32_t)osPriorityISR + 1;
}

static inline int32_t sched_get_priority_min(int32_t policy) {
    (void)policy;
    return (int32_t)osPriorityIdle;
}

static inline int32_t pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                                     void *(*start_routine)(void*), void *arg) {
    osThreadId_t tid;
    const osThreadAttr_t threadAttr = {
        .name = "elog_async",
        .stack_size = ELOG_ASYNC_OUTPUT_PTHREAD_STACK_SIZE,
        .priority = ELOG_ASYNC_OUTPUT_PTHREAD_PRIORITY,
    };

    (void)attr;

    tid = osThreadNew((osThreadFunc_t)start_routine, arg, &threadAttr);
    if (tid != NULL) {
        if (thread) {
            *thread = (pthread_t)tid;
        }
        return 0;
    }
    return -1;
}

static inline int32_t pthread_join(pthread_t thread, void **retval) {
    (void)retval;
    if (thread != NULL) {
        osThreadTerminate(thread);
        osDelay(10);
    }
    return 0;
}

static inline int32_t sem_init(sem_t *sem, int32_t pshared, uint32_t value) {
    (void)pshared;
    if (sem) {
        uint32_t max_count = (value > 0) ? value : 256;
        *sem = osSemaphoreNew(max_count, value, NULL);
        if (*sem != NULL) {
            return 0;
        }
    }
    return -1;
}

static inline int32_t sem_destroy(sem_t *sem) {
    if (sem && *sem != NULL) {
        osSemaphoreDelete(*sem);
        *sem = NULL;
        return 0;
    }
    return -1;
}

static inline int32_t sem_wait(sem_t *sem) {
    osStatus_t status;
    if (sem && *sem != NULL) {
        status = osSemaphoreAcquire(*sem, osWaitForever);
        return (status == osOK) ? 0 : -1;
    }
    return -1;
}

static inline int32_t sem_post(sem_t *sem) {
    osStatus_t status;
    if (sem && *sem != NULL) {
        status = osSemaphoreRelease(*sem);
        return (status == osOK) ? 0 : -1;
    }
    return -1;
}

static inline int32_t sem_trywait(sem_t *sem) {
    osStatus_t status;
    if (sem && *sem != NULL) {
        status = osSemaphoreAcquire(*sem, 0U);
        return (status == osOK) ? 0 : -1;
    }
    return -1;
}

#ifdef __cplusplus
}
#endif

#endif /* __ELOG_PTHREAD_PORT_H__ */