#include <math.h>
#include <string.h>

#include "check.h"
#include "mbox/pcm.h"

int main(void)
{
    /* Known big-endian 24-bit encodings. */
    const uint8_t be[] = {0x7f, 0xff, 0xff, 0x80, 0x00, 0x00, 0x00, 0x00, 0x01, 0xff, 0xff, 0xff};
    float f[4];
    mbox_pcm_decode(MBOX_PCM_S24_3BE, be, f, 4);
    CHECK(fabsf(f[0] - 8388607.0f / 8388608.0f) < 1e-9f);
    CHECK(f[1] == -1.0f);
    CHECK(f[2] == 1.0f / 8388608.0f);
    CHECK(f[3] == -1.0f / 8388608.0f);

    /* Round trip and clipping in every format. */
    const float src[] = {0.0f, 0.5f, -0.5f, 0.25f, 1.5f, -1.5f};
    const mbox_pcm_format fmts[] = {MBOX_PCM_S24_3BE, MBOX_PCM_S24_3LE, MBOX_PCM_S16_LE};
    for (size_t k = 0; k < 3; k++) {
        uint8_t buf[6 * 3];
        float back[6];
        mbox_pcm_encode(fmts[k], src, buf, 6);
        mbox_pcm_decode(fmts[k], buf, back, 6);
        for (int i = 0; i < 4; i++)
            CHECK(back[i] == src[i]);
        CHECK(back[4] > 0.9999f && back[4] < 1.0f);
        CHECK(back[5] == -1.0f);
    }

    /* Byte order differs between BE and LE. */
    uint8_t a[3], b[3];
    float x = 0.25f;
    mbox_pcm_encode(MBOX_PCM_S24_3BE, &x, a, 1);
    mbox_pcm_encode(MBOX_PCM_S24_3LE, &x, b, 1);
    CHECK(a[0] == 0x20 && a[1] == 0 && a[2] == 0);
    CHECK(b[0] == 0 && b[1] == 0 && b[2] == 0x20);
    return CHECK_DONE();
}
