#include <math.h>
#include <stdlib.h>

#include "check.h"
#include "mbox/clock.h"

int main(void)
{
    /* Device runs 0.02 % fast; observations jitter by up to +-400 us. */
    const double tps = 1e9, rate = 48000.0, true_rate = rate * 1.0002;
    mbox_clock c;
    mbox_clock_init(&c, rate, tps);
    srand(1);
    int64_t s = 0;
    for (int ms = 0; ms < 20000; ms++) {
        s += 48;
        double t = 5e9 + (double)s / true_rate * tps;
        double jitter = ((double)rand() / RAND_MAX - 0.5) * 800e3;
        mbox_clock_observe(&c, s, (uint64_t)(t + jitter));
    }
    double tpf = tps / true_rate;
    CHECK(fabs(c.ticks_per_frame - tpf) / tpf < 1e-5);
    double truth = 5e9 + (double)s / true_rate * tps;
    CHECK(fabs((double)mbox_clock_host_time(&c, s) - truth) < 100e3); /* < 100 us */
    CHECK(c.resets == 0);

    /* A large jump (device replugged) re-locks instead of slewing. */
    mbox_clock_observe(&c, s + 48, (uint64_t)(truth + 1e9));
    CHECK(c.resets == 1);
    CHECK(mbox_clock_host_time(&c, s + 48) == (uint64_t)(truth + 1e9));
    return CHECK_DONE();
}
