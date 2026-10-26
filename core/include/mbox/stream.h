/*
 * Isochronous streaming engine.
 *
 * Runs entirely from libusb callbacks: the caller must keep calling
 * libusb_handle_events*() on the context (mboxd does so on a dedicated
 * thread). Callbacks are invoked on that thread.
 *
 * Clock: the capture endpoint is synchronous to the device clock, so the
 * number of frames in each capture packet *is* the device clock. Playback
 * packets mirror those sizes (implicit feedback), which keeps the async
 * playback endpoint fed at exactly the device's rate. Output-only models
 * fall back to the nominal rate.
 */
#ifndef MBOX_STREAM_H
#define MBOX_STREAM_H

#include <stddef.h>
#include <stdint.h>

#include "mbox/device.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    void *ctx;
    /* Device clock advanced by `frames`, the last of which was sampled at
     * about `host_time`. Called before capture() for the same packet. */
    void (*clock)(void *ctx, uint32_t frames, uint64_t host_time);
    void (*capture)(void *ctx, const float *frames, uint32_t n);
    void (*playback)(void *ctx, float *frames, uint32_t n);
    void (*midi_in)(void *ctx, const uint8_t *bytes, size_t n);
    /* The device went away or a transfer failed fatally. */
    void (*fatal)(void *ctx, int err);
} mbox_stream_callbacks;

typedef struct {
    uint32_t transfers;            /* in flight per direction, default 3 */
    uint32_t packets_per_transfer; /* 1 packet = 1 ms at full speed, default 8 */
} mbox_stream_config;

typedef struct mbox_stream mbox_stream;

int mbox_stream_start(mbox_device *d, uint32_t rate, const mbox_stream_config *cfg,
                      const mbox_stream_callbacks *cb, mbox_stream **out);
/* Cancels all transfers and waits for them; the event loop must keep
 * running on another thread or this call drives it itself. */
void mbox_stream_stop(mbox_stream *s);

/* Frames queued towards the device ahead of the device's play position. */
uint32_t mbox_stream_output_latency(const mbox_stream *s);
/* Frames between the device's capture point and capture(). */
uint32_t mbox_stream_input_latency(const mbox_stream *s);

/* Thread-safe. Returns bytes accepted. */
size_t mbox_stream_midi_send(mbox_stream *s, const uint8_t *bytes, size_t n);

/* Spreads a fractional per-millisecond frame count evenly, e.g. 44.1 kHz
 * gives nine packets of 44 frames then one of 45. Exposed for tests. */
typedef struct {
    uint32_t rate;
    uint32_t acc;
} mbox_pacer;

void mbox_pacer_init(mbox_pacer *p, uint32_t rate);
uint32_t mbox_pacer_next(mbox_pacer *p);

#ifdef __cplusplus
}
#endif

#endif
