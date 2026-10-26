#include <stdlib.h>

#include "check.h"
#include "mbox/shm.h"

int main(void)
{
    size_t size = mbox_shm_size(4096, 2, 2);
    void *base = aligned_alloc(64, (size + 63) & ~(size_t)63);
    mbox_shm_init(base, 4096, 2, 2, 512);
    CHECK(mbox_shm_validate(base, size));
    CHECK(!mbox_shm_validate(base, size - 1));

    mbox_shm_header *h = base;
    mbox_anchor a;
    CHECK(!mbox_shm_read_anchor(h, &a)); /* nothing published yet */
    mbox_anchor w = {.sample_time = 1024, .host_time = 99, .generation = 3};
    mbox_shm_publish_anchor(h, &w);
    CHECK(mbox_shm_read_anchor(h, &a));
    CHECK(a.sample_time == 1024 && a.host_time == 99 && a.generation == 3);

    /* Rings are disjoint and inside the region. */
    mbox_tl_ring in = mbox_shm_input_ring(base), out = mbox_shm_output_ring(base);
    CHECK((char *)in.data + 4096 * 2 * sizeof(float) <= (char *)out.data);
    CHECK((char *)out.data + 4096 * 2 * sizeof(float) <= (char *)base + size);
    CHECK(((uintptr_t)in.data & 63) == 0 && ((uintptr_t)out.data & 63) == 0);

    /* Active channels may not exceed the layout. */
    h->in_channels = 3;
    CHECK(!mbox_shm_validate(base, size));
    h->in_channels = 0; /* output-only device (Mbox 2 Micro) */
    CHECK(mbox_shm_validate(base, size));

    /* MIDI FIFO: wraps, and refuses to overrun. */
    uint8_t buf[MBOX_MIDI_RING_SIZE + 8], got[16];
    for (size_t i = 0; i < sizeof buf; i++)
        buf[i] = (uint8_t)i;
    CHECK(mbox_byte_ring_write(&h->midi_in, buf, sizeof buf) == MBOX_MIDI_RING_SIZE);
    CHECK(mbox_byte_ring_write(&h->midi_in, buf, 1) == 0);
    CHECK(mbox_byte_ring_read(&h->midi_in, got, 4) == 4 && got[3] == 3);
    CHECK(mbox_byte_ring_write(&h->midi_in, buf + 100, 3) == 3);
    uint8_t all[MBOX_MIDI_RING_SIZE];
    CHECK(mbox_byte_ring_read(&h->midi_in, all, sizeof all) == MBOX_MIDI_RING_SIZE - 1);
    CHECK(all[MBOX_MIDI_RING_SIZE - 2] == 102);
    free(base);
    return CHECK_DONE();
}
