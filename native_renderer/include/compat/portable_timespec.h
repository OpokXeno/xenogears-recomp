#ifndef XG_COMPAT_PORTABLE_TIMESPEC_H
#define XG_COMPAT_PORTABLE_TIMESPEC_H

#include <time.h>

#if !defined(TIME_UTC)

#include <windows.h>

#define TIME_UTC 1

static inline int timespec_get(struct timespec *ts, int base) {
    FILETIME file_time;
    ULARGE_INTEGER wide_time;
    unsigned long long ticks_since_1601;

    if (base != TIME_UTC || !ts) {
        return 0;
    }

    GetSystemTimeAsFileTime(&file_time);
    wide_time.LowPart = file_time.dwLowDateTime;
    wide_time.HighPart = file_time.dwHighDateTime;

    ticks_since_1601 = wide_time.QuadPart - 116444736000000000ULL;
    ts->tv_sec = (time_t)(ticks_since_1601 / 10000000ULL);
    ts->tv_nsec = (long)((ticks_since_1601 % 10000000ULL) * 100);
    return base;
}

#endif

#endif
