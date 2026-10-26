/*
 * Layout of the memory region shared between mboxd (owns the USB device)
 * and the Core Audio plug-in (runs inside coreaudiod).
 *
 *   [mbox_shm_header][input ring floats][output ring floats]
 *
 * Both rings are timeline rings (see ring.h) indexed by device sample
 * time. The device clock is published as a seqlock-protected anchor so
 * the plug-in can answer GetZeroTimeStamp without talking to the daemon.
 */
#ifndef MBOX_SHM_H
#define MBOX_SHM_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "mbox/ring.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MBOX_SHM_MAGIC 0x4d424f58u /* 'MBOX' */
#define MBOX_SHM_VERSION 1u
#define MBOX_MACH_SERVICE "com.rockgxyz.mboxd"
#define MBOX_SHM_POSIX_NAME "/mboxd.audio"

/* Bytes of MIDI buffered in each direction; power of two. */
#define MBOX_MIDI_RING_SIZE 4096u

enum {
    MBOX_STATE_ABSENT = 0,  /* no device, plug-in should render silence */
    MBOX_STATE_RUNNING = 1, /* anchors are valid */
};

typedef struct {
    int64_t sample_time; /* multiple of zts_period */
    uint64_t host_time;  /* mach_absolute_time() / CLOCK_MONOTONIC ns */
    uint64_t generation; /* bumps when the clock restarts */
} mbox_anchor;

/* Single-producer single-consumer byte FIFO for raw MIDI. */
typedef struct {
    _Atomic uint32_t head; /* written by the producer */
    _Atomic uint32_t tail; /* written by the consumer */
    uint8_t data[MBOX_MIDI_RING_SIZE];
} mbox_byte_ring;

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t total_size;
    uint32_t ring_frames;
    uint32_t in_stride;     /* samples per frame in the input ring */
    uint32_t out_stride;
    uint32_t zts_period;    /* frames between clock anchors */
    uint32_t in_offset;     /* byte offset of the input ring */
    uint32_t out_offset;    /* byte offset of the output ring */
    /* Device fields: rewritten by mboxd whenever a device is attached,
     * while state is MBOX_STATE_ABSENT. */
    uint32_t sample_rate;
    uint32_t in_channels;   /* active channels, <= stride; 0 = no input */
    uint32_t out_channels;
    uint32_t out_latency;   /* frames queued in USB transfers ahead of the device */
    uint32_t in_latency;    /* frames between ADC and the ring */
    char model_id[16];
    char model_name[48];
    _Atomic uint32_t state;
    _Atomic uint32_t seq; /* odd while the anchor is being written */
    mbox_anchor anchor;
    /* Output cursor of the daemon: frames before this time have already
     * been sent to the device. The plug-in uses it to detect underruns. */
    _Atomic int64_t out_cursor;
    _Atomic uint32_t client_io_count; /* plug-in StartIO refcount */
    uint32_t reserved[13];
    mbox_byte_ring midi_in;  /* device -> CoreMIDI agent */
    mbox_byte_ring midi_out; /* CoreMIDI agent -> device */
} mbox_shm_header;

size_t mbox_shm_size(uint32_t ring_frames, uint32_t in_stride, uint32_t out_stride);
/* Lays out a fresh region; the device fields start as "absent". */
void mbox_shm_init(void *base, uint32_t ring_frames, uint32_t in_stride, uint32_t out_stride,
                   uint32_t zts_period);
bool mbox_shm_validate(const void *base, size_t mapped_size);

mbox_tl_ring mbox_shm_input_ring(void *base);
mbox_tl_ring mbox_shm_output_ring(void *base);

size_t mbox_byte_ring_write(mbox_byte_ring *r, const uint8_t *src, size_t n);
size_t mbox_byte_ring_read(mbox_byte_ring *r, uint8_t *dst, size_t cap);

void mbox_shm_publish_anchor(mbox_shm_header *h, const mbox_anchor *a);
/* Returns false if no consistent anchor could be read (writer busy). */
bool mbox_shm_read_anchor(const mbox_shm_header *h, mbox_anchor *out);

#ifdef __cplusplus
}
#endif

#endif
