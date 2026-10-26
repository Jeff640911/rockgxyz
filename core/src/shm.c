#include "mbox/shm.h"

#include <string.h>

#define ALIGN64(x) (((x) + 63u) & ~(size_t)63u)

static size_t ring_bytes(uint32_t frames, uint32_t channels)
{
    return ALIGN64((size_t)frames * channels * sizeof(float));
}

size_t mbox_shm_size(uint32_t ring_frames, uint32_t in_stride, uint32_t out_stride)
{
    return ALIGN64(sizeof(mbox_shm_header)) + ring_bytes(ring_frames, in_stride) +
           ring_bytes(ring_frames, out_stride);
}

void mbox_shm_init(void *base, uint32_t ring_frames, uint32_t in_stride, uint32_t out_stride,
                   uint32_t zts_period)
{
    size_t size = mbox_shm_size(ring_frames, in_stride, out_stride);
    memset(base, 0, size);
    mbox_shm_header *h = base;
    h->magic = MBOX_SHM_MAGIC;
    h->version = MBOX_SHM_VERSION;
    h->total_size = (uint32_t)size;
    h->ring_frames = ring_frames;
    h->in_stride = in_stride;
    h->out_stride = out_stride;
    h->zts_period = zts_period;
    h->in_offset = (uint32_t)ALIGN64(sizeof(mbox_shm_header));
    h->out_offset = h->in_offset + (uint32_t)ring_bytes(ring_frames, in_stride);
    atomic_store(&h->state, MBOX_STATE_ABSENT);
}

bool mbox_shm_validate(const void *base, size_t mapped_size)
{
    const mbox_shm_header *h = base;
    if (mapped_size < sizeof *h || h->magic != MBOX_SHM_MAGIC || h->version != MBOX_SHM_VERSION)
        return false;
    if (h->total_size > mapped_size || h->ring_frames == 0 || h->zts_period == 0)
        return false;
    if (h->in_channels > h->in_stride || h->out_channels > h->out_stride)
        return false;
    return h->total_size == mbox_shm_size(h->ring_frames, h->in_stride, h->out_stride);
}

mbox_tl_ring mbox_shm_input_ring(void *base)
{
    mbox_shm_header *h = base;
    return (mbox_tl_ring){(float *)((char *)base + h->in_offset), h->ring_frames, h->in_stride};
}

mbox_tl_ring mbox_shm_output_ring(void *base)
{
    mbox_shm_header *h = base;
    return (mbox_tl_ring){(float *)((char *)base + h->out_offset), h->ring_frames, h->out_stride};
}

size_t mbox_byte_ring_write(mbox_byte_ring *r, const uint8_t *src, size_t n)
{
    uint32_t head = atomic_load_explicit(&r->head, memory_order_relaxed);
    uint32_t tail = atomic_load_explicit(&r->tail, memory_order_acquire);
    size_t room = MBOX_MIDI_RING_SIZE - (head - tail);
    if (n > room)
        n = room;
    for (size_t i = 0; i < n; i++)
        r->data[(head + i) & (MBOX_MIDI_RING_SIZE - 1)] = src[i];
    atomic_store_explicit(&r->head, head + (uint32_t)n, memory_order_release);
    return n;
}

size_t mbox_byte_ring_read(mbox_byte_ring *r, uint8_t *dst, size_t cap)
{
    uint32_t tail = atomic_load_explicit(&r->tail, memory_order_relaxed);
    uint32_t head = atomic_load_explicit(&r->head, memory_order_acquire);
    size_t n = head - tail;
    if (n > cap)
        n = cap;
    for (size_t i = 0; i < n; i++)
        dst[i] = r->data[(tail + i) & (MBOX_MIDI_RING_SIZE - 1)];
    atomic_store_explicit(&r->tail, tail + (uint32_t)n, memory_order_release);
    return n;
}

void mbox_shm_publish_anchor(mbox_shm_header *h, const mbox_anchor *a)
{
    uint32_t s = atomic_load_explicit(&h->seq, memory_order_relaxed);
    atomic_store_explicit(&h->seq, s + 1, memory_order_relaxed);
    atomic_thread_fence(memory_order_release);
    h->anchor = *a;
    atomic_store_explicit(&h->seq, s + 2, memory_order_release);
}

bool mbox_shm_read_anchor(const mbox_shm_header *h, mbox_anchor *out)
{
    for (int tries = 0; tries < 64; tries++) {
        uint32_t before = atomic_load_explicit(&h->seq, memory_order_acquire);
        if (before & 1u)
            continue;
        mbox_anchor copy = h->anchor;
        atomic_thread_fence(memory_order_acquire);
        if (atomic_load_explicit(&h->seq, memory_order_relaxed) == before) {
            *out = copy;
            return before != 0; /* seq 0 = never published */
        }
    }
    return false;
}
