#include "check.h"
#include "mbox/stream.h"

int main(void)
{
    mbox_pacer p;
    mbox_pacer_init(&p, 48000);
    for (int i = 0; i < 100; i++)
        CHECK(mbox_pacer_next(&p) == 48);

    mbox_pacer_init(&p, 44100);
    uint32_t total = 0, min = 1000, max = 0;
    for (int i = 0; i < 1000; i++) {
        uint32_t f = mbox_pacer_next(&p);
        total += f;
        if (f < min)
            min = f;
        if (f > max)
            max = f;
    }
    CHECK(total == 44100);
    CHECK(min == 44 && max == 45);
    return CHECK_DONE();
}
