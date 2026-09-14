#ifndef XG_THREADS_H
#define XG_THREADS_H

#if !defined(_WIN32)

#include <threads.h>

#else

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <process.h>
#include <stdint.h>
#include <stdlib.h>
#include <time.h>

enum {
    thrd_success = 0,
    thrd_timedout = 1,
    thrd_error = 2,
    mtx_plain = 0,
};

typedef CRITICAL_SECTION mtx_t;
typedef CONDITION_VARIABLE cnd_t;

typedef struct thrd_t {
    HANDLE handle;
    DWORD id;
} thrd_t;

typedef struct XgThreadStart {
    int (*function)(void *);
    void *argument;
} XgThreadStart;

static unsigned int __stdcall xg_thread_entry(void *opaque) {
    XgThreadStart *start = (XgThreadStart *)opaque;
    int (*function)(void *) = start->function;
    void *argument = start->argument;
    int result;

    free(start);
    result = function(argument);
    return (unsigned int)result;
}

static int mtx_init(mtx_t *mutex, int type) {
    (void)type;
    InitializeCriticalSection(mutex);
    return thrd_success;
}

static int mtx_lock(mtx_t *mutex) {
    EnterCriticalSection(mutex);
    return thrd_success;
}

static int mtx_unlock(mtx_t *mutex) {
    LeaveCriticalSection(mutex);
    return thrd_success;
}

static void mtx_destroy(mtx_t *mutex) {
    DeleteCriticalSection(mutex);
}

static int cnd_init(cnd_t *condition) {
    InitializeConditionVariable(condition);
    return thrd_success;
}

static int cnd_signal(cnd_t *condition) {
    WakeConditionVariable(condition);
    return thrd_success;
}

static int cnd_broadcast(cnd_t *condition) {
    WakeAllConditionVariable(condition);
    return thrd_success;
}

static int cnd_wait(cnd_t *condition, mtx_t *mutex) {
    return SleepConditionVariableCS(condition, mutex, INFINITE)
        ? thrd_success : thrd_error;
}

static int cnd_timedwait(cnd_t *condition, mtx_t *mutex,
                         const struct timespec *deadline) {
    struct timespec now;
    time_t seconds;
    long nanoseconds;
    uint64_t milliseconds;
    DWORD timeout;

    if (timespec_get(&now, TIME_UTC) != TIME_UTC) return thrd_error;
    seconds = deadline->tv_sec - now.tv_sec;
    nanoseconds = deadline->tv_nsec - now.tv_nsec;
    if (nanoseconds < 0) {
        seconds--;
        nanoseconds += 1000000000L;
    }
    if (seconds < 0) {
        timeout = 0u;
    } else {
        milliseconds = (uint64_t)seconds * 1000u +
            ((uint64_t)nanoseconds + 999999u) / 1000000u;
        timeout = milliseconds >= (uint64_t)(INFINITE - 1u)
            ? INFINITE - 1u : (DWORD)milliseconds;
    }
    if (SleepConditionVariableCS(condition, mutex, timeout))
        return thrd_success;
    return GetLastError() == ERROR_TIMEOUT ? thrd_timedout : thrd_error;
}

static void cnd_destroy(cnd_t *condition) {
    (void)condition;
}

static thrd_t thrd_current(void) {
    thrd_t thread = {NULL, GetCurrentThreadId()};
    return thread;
}

static int thrd_equal(thrd_t left, thrd_t right) {
    return left.id == right.id;
}

static int thrd_create(thrd_t *thread, int (*function)(void *), void *argument) {
    XgThreadStart *start = (XgThreadStart *)malloc(sizeof(*start));
    unsigned int thread_id;

    if (start == NULL) return thrd_error;
    start->function = function;
    start->argument = argument;
    thread->handle = (HANDLE)_beginthreadex(
        NULL, 0u, xg_thread_entry, start, 0u, &thread_id);
    if (thread->handle == NULL) {
        free(start);
        thread->id = 0u;
        return thrd_error;
    }
    thread->id = (DWORD)thread_id;
    return thrd_success;
}

static int thrd_join(thrd_t thread, int *result) {
    DWORD exit_code;

    if (thread.handle == NULL ||
        WaitForSingleObject(thread.handle, INFINITE) != WAIT_OBJECT_0 ||
        !GetExitCodeThread(thread.handle, &exit_code)) {
        if (thread.handle != NULL) CloseHandle(thread.handle);
        return thrd_error;
    }
    CloseHandle(thread.handle);
    if (result != NULL) *result = (int)exit_code;
    return thrd_success;
}

static void thrd_yield(void) {
    if (!SwitchToThread()) Sleep(0u);
}

#endif

#endif
