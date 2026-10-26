#include "mbox/pcm.h"

#define S24_SCALE 8388608.0f /* 2^23 */
#define S16_SCALE 32768.0f

static int32_t sign_extend24(uint32_t v)
{
    return (int32_t)(v << 8) >> 8;
}

static int32_t quantize(float x, float scale, int32_t max)
{
    float v = x * scale;
    if (v >= (float)max)
        return max;
    if (v <= (float)(-max - 1))
        return -max - 1;
    /* round half away from zero */
    return (int32_t)(v < 0.0f ? v - 0.5f : v + 0.5f);
}

void mbox_pcm_decode(mbox_pcm_format fmt, const uint8_t *src, float *dst, size_t samples)
{
    switch (fmt) {
    case MBOX_PCM_S24_3BE:
        for (size_t i = 0; i < samples; i++, src += 3)
            dst[i] = (float)sign_extend24((uint32_t)src[0] << 16 | (uint32_t)src[1] << 8 | src[2]) / S24_SCALE;
        break;
    case MBOX_PCM_S24_3LE:
        for (size_t i = 0; i < samples; i++, src += 3)
            dst[i] = (float)sign_extend24((uint32_t)src[2] << 16 | (uint32_t)src[1] << 8 | src[0]) / S24_SCALE;
        break;
    case MBOX_PCM_S16_LE:
        for (size_t i = 0; i < samples; i++, src += 2)
            dst[i] = (float)(int16_t)((uint16_t)src[1] << 8 | src[0]) / S16_SCALE;
        break;
    }
}

void mbox_pcm_encode(mbox_pcm_format fmt, const float *src, uint8_t *dst, size_t samples)
{
    switch (fmt) {
    case MBOX_PCM_S24_3BE:
        for (size_t i = 0; i < samples; i++, dst += 3) {
            uint32_t v = (uint32_t)quantize(src[i], S24_SCALE, 0x7fffff);
            dst[0] = (uint8_t)(v >> 16);
            dst[1] = (uint8_t)(v >> 8);
            dst[2] = (uint8_t)v;
        }
        break;
    case MBOX_PCM_S24_3LE:
        for (size_t i = 0; i < samples; i++, dst += 3) {
            uint32_t v = (uint32_t)quantize(src[i], S24_SCALE, 0x7fffff);
            dst[0] = (uint8_t)v;
            dst[1] = (uint8_t)(v >> 8);
            dst[2] = (uint8_t)(v >> 16);
        }
        break;
    case MBOX_PCM_S16_LE:
        for (size_t i = 0; i < samples; i++, dst += 2) {
            uint16_t v = (uint16_t)quantize(src[i], S16_SCALE, 0x7fff);
            dst[0] = (uint8_t)v;
            dst[1] = (uint8_t)(v >> 8);
        }
        break;
    }
}
