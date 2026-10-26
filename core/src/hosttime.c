#include "mbox/hosttime.h"

#ifdef __APPLE__
#include <mach/mach_time.h>

uint64_t mbox_host_time_now(void)
{
    return mach_absolute_time();
}

double mbox_host_ticks_per_second(void)
{
    mach_timebase_info_data_t tb;
    mach_timebase_info(&tb);
    return 1e9 * (double)tb.denom / (double)tb.numer;
}

#else
#include <time.h>

uint64_t mbox_host_time_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

double mbox_host_ticks_per_second(void)
{
    return 1e9;
}
#endif
