/*
 * Smooths the jittery (packet completion time, frames) observations from
 * the USB callbacks into a steady device clock, using a second-order
 * delay-locked loop. The plug-in's zero time stamps come from this.
 */
#ifndef MBOX_CLOCK_H
#define MBOX_CLOCK_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    double ticks_per_frame;   /* current estimate */
    double nominal_tpf;
    double base_host;         /* filtered host time at base_sample */
    int64_t base_sample;
    double gain_phase, gain_rate;
    double reset_threshold;   /* ticks of error that restart the loop */
    bool locked;
    uint32_t resets;
} mbox_clock;

void mbox_clock_init(mbox_clock *c, double sample_rate, double host_ticks_per_second);
/* `sample_time` is the device time of the frame observed at `host_time`. */
void mbox_clock_observe(mbox_clock *c, int64_t sample_time, uint64_t host_time);
/* Filtered host time at which `sample_time` occurs (or occurred). */
uint64_t mbox_clock_host_time(const mbox_clock *c, int64_t sample_time);

#ifdef __cplusplus
}
#endif

#endif
