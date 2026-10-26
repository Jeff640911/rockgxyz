/*
 * Timeline ring buffer.
 *
 * Frames are stored at index (sample_time mod capacity), so the producer
 * and the consumer never have to agree on read/write pointers: both just
 * address the shared device timeline. This is what lets the USB daemon
 * and the Core Audio plug-in, which live in different processes, exchange
 * audio without locks.
 */
#ifndef MBOX_RING_H
#define MBOX_RING_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    float *data;               /* capacity_frames * channels samples */
    uint32_t capacity_frames;  /* any value > 0 */
    uint32_t channels;
} mbox_tl_ring;

void mbox_tl_write(const mbox_tl_ring *r, int64_t sample_time, const float *src, uint32_t frames);
void mbox_tl_read(const mbox_tl_ring *r, int64_t sample_time, float *dst, uint32_t frames);
/* Read, then zero the frames so a stalled producer yields silence rather
 * than replaying stale audio. */
void mbox_tl_consume(const mbox_tl_ring *r, int64_t sample_time, float *dst, uint32_t frames);
void mbox_tl_clear(const mbox_tl_ring *r, int64_t sample_time, uint32_t frames);

#ifdef __cplusplus
}
#endif

#endif
