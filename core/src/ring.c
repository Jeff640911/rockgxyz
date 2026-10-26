#include "mbox/ring.h"

#include <string.h>

static uint32_t wrap(const mbox_tl_ring *r, int64_t sample_time)
{
    int64_t m = sample_time % (int64_t)r->capacity_frames;
    return (uint32_t)(m < 0 ? m + r->capacity_frames : m);
}

/* Calls fn on each contiguous span covered by [sample_time, +frames). */
#define FOR_EACH_SPAN(r, sample_time, frames, body)                                  \
    do {                                                                             \
        uint32_t remaining_ = (frames) > (r)->capacity_frames ? (r)->capacity_frames \
                                                              : (frames);            \
        uint32_t skip_ = (frames) - remaining_;                                      \
        uint32_t pos_ = wrap((r), (sample_time) + skip_);                            \
        uint32_t done_ = skip_;                                                      \
        while (remaining_) {                                                         \
            uint32_t span = (r)->capacity_frames - pos_;                             \
            if (span > remaining_)                                                   \
                span = remaining_;                                                   \
            float *slot = (r)->data + (size_t)pos_ * (r)->channels;                  \
            size_t offset = (size_t)done_ * (r)->channels;                           \
            size_t count = (size_t)span * (r)->channels;                             \
            body;                                                                    \
            pos_ = 0;                                                                \
            done_ += span;                                                           \
            remaining_ -= span;                                                      \
        }                                                                            \
    } while (0)

void mbox_tl_write(const mbox_tl_ring *r, int64_t sample_time, const float *src, uint32_t frames)
{
    /* Writing more than one ring's worth keeps only the newest frames. */
    FOR_EACH_SPAN(r, sample_time, frames, memcpy(slot, src + offset, count * sizeof(float)));
}

void mbox_tl_read(const mbox_tl_ring *r, int64_t sample_time, float *dst, uint32_t frames)
{
    if (frames > r->capacity_frames) {
        memset(dst, 0, (size_t)(frames - r->capacity_frames) * r->channels * sizeof(float));
    }
    FOR_EACH_SPAN(r, sample_time, frames, memcpy(dst + offset, slot, count * sizeof(float)));
}

void mbox_tl_consume(const mbox_tl_ring *r, int64_t sample_time, float *dst, uint32_t frames)
{
    if (frames > r->capacity_frames) {
        memset(dst, 0, (size_t)(frames - r->capacity_frames) * r->channels * sizeof(float));
    }
    FOR_EACH_SPAN(r, sample_time, frames, {
        memcpy(dst + offset, slot, count * sizeof(float));
        memset(slot, 0, count * sizeof(float));
    });
}

void mbox_tl_clear(const mbox_tl_ring *r, int64_t sample_time, uint32_t frames)
{
    FOR_EACH_SPAN(r, sample_time, frames, (void)offset; memset(slot, 0, count * sizeof(float)));
}
