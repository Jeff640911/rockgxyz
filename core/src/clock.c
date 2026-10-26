#include "mbox/clock.h"

#include <math.h>

void mbox_clock_init(mbox_clock *c, double sample_rate, double host_ticks_per_second)
{
    c->nominal_tpf = host_ticks_per_second / sample_rate;
    c->ticks_per_frame = c->nominal_tpf;
    c->base_host = 0;
    c->base_sample = 0;
    /* Tuned for ~1 ms observations with up to +-0.5 ms of USB scheduling
     * jitter: phase error stays within tens of microseconds and the rate
     * estimate within a few ppm (see tests/test_clock.c). */
    c->gain_phase = 0.002;
    c->gain_rate = 0.000001;
    c->reset_threshold = host_ticks_per_second * 0.010; /* 10 ms */
    c->locked = false;
    c->resets = 0;
}

void mbox_clock_observe(mbox_clock *c, int64_t sample_time, uint64_t host_time)
{
    double h = (double)host_time;
    if (!c->locked) {
        c->base_host = h;
        c->base_sample = sample_time;
        c->ticks_per_frame = c->nominal_tpf;
        c->locked = true;
        return;
    }
    int64_t frames = sample_time - c->base_sample;
    if (frames <= 0)
        return;
    double predicted = c->base_host + (double)frames * c->ticks_per_frame;
    double err = h - predicted;
    if (fabs(err) > c->reset_threshold) {
        c->locked = false;
        c->resets++;
        mbox_clock_observe(c, sample_time, host_time);
        return;
    }
    c->base_host = predicted + c->gain_phase * err;
    c->base_sample = sample_time;
    c->ticks_per_frame += c->gain_rate * err / (double)frames;
    /* A real converter is never off by more than a fraction of a percent. */
    double lo = c->nominal_tpf * 0.995, hi = c->nominal_tpf * 1.005;
    if (c->ticks_per_frame < lo)
        c->ticks_per_frame = lo;
    if (c->ticks_per_frame > hi)
        c->ticks_per_frame = hi;
}

uint64_t mbox_clock_host_time(const mbox_clock *c, int64_t sample_time)
{
    double t = c->base_host + (double)(sample_time - c->base_sample) * c->ticks_per_frame;
    return t > 0 ? (uint64_t)t : 0;
}
