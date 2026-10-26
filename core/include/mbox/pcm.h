/* Conversion between the device's packed integer samples and the
 * interleaved float32 frames Core Audio works with. */
#ifndef MBOX_PCM_H
#define MBOX_PCM_H

#include <stddef.h>
#include <stdint.h>

#include "mbox/models.h"

#ifdef __cplusplus
extern "C" {
#endif

/* `samples` is frames * channels. */
void mbox_pcm_decode(mbox_pcm_format fmt, const uint8_t *src, float *dst, size_t samples);
void mbox_pcm_encode(mbox_pcm_format fmt, const float *src, uint8_t *dst, size_t samples);

#ifdef __cplusplus
}
#endif

#endif
