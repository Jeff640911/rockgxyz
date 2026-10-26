#include <string.h>

#include "check.h"
#include "mbox/ring.h"

int main(void)
{
    float storage[10 * 2];
    mbox_tl_ring r = {storage, 10, 2};
    memset(storage, 0, sizeof storage);

    /* Write across the wrap point and read it back. */
    float in[8 * 2], out[8 * 2];
    for (int i = 0; i < 16; i++)
        in[i] = (float)(i + 1);
    mbox_tl_write(&r, 7, in, 8);
    mbox_tl_read(&r, 7, out, 8);
    CHECK(memcmp(in, out, sizeof in) == 0);
    CHECK(storage[7 * 2] == 1.0f && storage[0] == 7.0f);

    /* Negative sample times wrap correctly. */
    mbox_tl_write(&r, -3, in, 2);
    mbox_tl_read(&r, 7, out, 2);
    CHECK(out[0] == 1.0f && out[3] == 4.0f);

    /* consume() zeroes what it read. */
    mbox_tl_consume(&r, 7, out, 2);
    mbox_tl_read(&r, 7, out, 2);
    CHECK(out[0] == 0.0f && out[3] == 0.0f);

    /* Oversized writes keep the newest frames; oversized reads zero-fill. */
    float big[12 * 2];
    for (int i = 0; i < 24; i++)
        big[i] = (float)i;
    mbox_tl_write(&r, 0, big, 12);
    float bout[12 * 2];
    mbox_tl_read(&r, 0, bout, 12);
    CHECK(bout[0] == 0.0f && bout[3] == 0.0f);
    CHECK(bout[4] == 4.0f && bout[23] == 23.0f);

    mbox_tl_clear(&r, 0, 10);
    for (int i = 0; i < 20; i++)
        CHECK(storage[i] == 0.0f);
    return CHECK_DONE();
}
