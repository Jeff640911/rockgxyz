/* Host clock shared by mboxd and the plug-in: mach_absolute_time() on
 * macOS (the clock Core Audio time stamps use), CLOCK_MONOTONIC elsewhere. */
#ifndef MBOX_HOSTTIME_H
#define MBOX_HOSTTIME_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

uint64_t mbox_host_time_now(void);
double mbox_host_ticks_per_second(void);

#ifdef __cplusplus
}
#endif

#endif
