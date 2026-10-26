#include "mbox/stream.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "mbox/hosttime.h"
#include "mbox/midi.h"
#include "mbox/pcm.h"

#define PACKETS_PER_SECOND 1000u /* full-speed USB: one packet per 1 ms frame */
#define FEEDBACK_FIFO 256u       /* power of two */
#define MIDI_QUEUE 1024u
#define STOP_TIMEOUT_MS 2000

void mbox_pacer_init(mbox_pacer *p, uint32_t rate)
{
    p->rate = rate;
    p->acc = 0;
}

uint32_t mbox_pacer_next(mbox_pacer *p)
{
    p->acc += p->rate;
    uint32_t frames = p->acc / PACKETS_PER_SECOND;
    p->acc -= frames * PACKETS_PER_SECOND;
    return frames;
}

typedef struct {
    struct libusb_transfer *t;
    mbox_stream *s;
    bool in_flight;
} xfer;

struct mbox_stream {
    mbox_device *dev;
    libusb_device_handle *h;
    const mbox_model *m;
    mbox_stream_callbacks cb;
    mbox_stream_config cfg;
    uint32_t rate;
    unsigned in_frame_bytes, out_frame_bytes;
    unsigned in_max_packet, out_max_packet;
    double ticks_per_packet;

    xfer *cap, *play;
    xfer midi_in, midi_out;
    atomic_int in_flight;
    atomic_bool stopping;

    /* Event-thread only. */
    uint16_t fb[FEEDBACK_FIFO];
    uint32_t fb_head, fb_tail;
    mbox_pacer out_pacer; /* playback sizes when no feedback is queued */
    mbox_pacer gap_pacer; /* stand-in sizes for lost capture packets */
    float *scratch;

    pthread_mutex_t midi_lock;
    mbox_midi_parser midi_parser;
    uint8_t midi_q[MIDI_QUEUE];
    size_t midi_q_len;
    size_t midi_in_flight_len;
    unsigned midi_out_max;
};

static void fb_push(mbox_stream *s, uint32_t frames)
{
    if (s->fb_head - s->fb_tail == FEEDBACK_FIFO)
        s->fb_tail++; /* playback stalled: forget the oldest */
    s->fb[s->fb_head++ & (FEEDBACK_FIFO - 1)] = (uint16_t)frames;
}

static uint32_t next_out_frames(mbox_stream *s)
{
    if (s->fb_head != s->fb_tail)
        return s->fb[s->fb_tail++ & (FEEDBACK_FIFO - 1)];
    return mbox_pacer_next(&s->out_pacer);
}

static int submit(mbox_stream *s, xfer *x)
{
    int r = libusb_submit_transfer(x->t);
    if (r == 0) {
        x->in_flight = true;
        atomic_fetch_add(&s->in_flight, 1);
    }
    return r;
}

/* Common completion bookkeeping; returns false if the transfer must not be
 * resubmitted. */
static bool completed(mbox_stream *s, xfer *x)
{
    x->in_flight = false;
    atomic_fetch_sub(&s->in_flight, 1);
    if (atomic_load(&s->stopping) || x->t->status == LIBUSB_TRANSFER_CANCELLED)
        return false;
    if (x->t->status == LIBUSB_TRANSFER_NO_DEVICE) {
        if (s->cb.fatal)
            s->cb.fatal(s->cb.ctx, LIBUSB_ERROR_NO_DEVICE);
        return false;
    }
    return true;
}

static void resubmit(mbox_stream *s, xfer *x)
{
    int r = submit(s, x);
    if (r != 0 && s->cb.fatal)
        s->cb.fatal(s->cb.ctx, r);
}

/* ---- capture ---------------------------------------------------------- */

static void LIBUSB_CALL capture_done(struct libusb_transfer *t)
{
    xfer *x = t->user_data;
    mbox_stream *s = x->s;
    if (!completed(s, x))
        return;

    uint64_t now = mbox_host_time_now();
    unsigned ch = s->m->capture.channels;
    for (int i = 0; i < t->num_iso_packets; i++) {
        const struct libusb_iso_packet_descriptor *pd = &t->iso_packet_desc[i];
        bool ok = t->status == LIBUSB_TRANSFER_COMPLETED && pd->status == LIBUSB_TRANSFER_COMPLETED;
        uint32_t frames = ok ? pd->actual_length / s->in_frame_bytes : mbox_pacer_next(&s->gap_pacer);
        uint64_t host = now - (uint64_t)((t->num_iso_packets - 1 - i) * s->ticks_per_packet);

        if (s->cb.clock)
            s->cb.clock(s->cb.ctx, frames, host);
        if (ok)
            mbox_pcm_decode(s->m->format, libusb_get_iso_packet_buffer_simple(t, (unsigned)i), s->scratch,
                            (size_t)frames * ch);
        else /* keep the timeline continuous across a lost packet */
            memset(s->scratch, 0, (size_t)frames * ch * sizeof(float));
        if (s->cb.capture)
            s->cb.capture(s->cb.ctx, s->scratch, frames);
        fb_push(s, frames);
    }
    resubmit(s, x);
}

/* ---- playback --------------------------------------------------------- */

static void fill_playback(mbox_stream *s, struct libusb_transfer *t)
{
    unsigned ch = s->m->playback.channels;
    uint32_t max_frames = s->out_max_packet / s->out_frame_bytes;
    size_t offset = 0;
    for (int i = 0; i < t->num_iso_packets; i++) {
        uint32_t frames = next_out_frames(s);
        if (frames > max_frames)
            frames = max_frames;
        if (s->cb.playback)
            s->cb.playback(s->cb.ctx, s->scratch, frames);
        else
            memset(s->scratch, 0, (size_t)frames * ch * sizeof(float));
        mbox_pcm_encode(s->m->format, s->scratch, t->buffer + offset, (size_t)frames * ch);
        t->iso_packet_desc[i].length = frames * s->out_frame_bytes;
        offset += t->iso_packet_desc[i].length;
    }
    t->length = (int)offset;
}

static void LIBUSB_CALL playback_done(struct libusb_transfer *t)
{
    xfer *x = t->user_data;
    mbox_stream *s = x->s;
    if (!completed(s, x))
        return;

    if (!s->m->capture.endpoint) {
        /* Output-only: the playback endpoint is the clock. */
        uint64_t now = mbox_host_time_now();
        for (int i = 0; i < t->num_iso_packets; i++) {
            uint64_t host = now - (uint64_t)((t->num_iso_packets - 1 - i) * s->ticks_per_packet);
            if (s->cb.clock)
                s->cb.clock(s->cb.ctx, t->iso_packet_desc[i].length / s->out_frame_bytes, host);
        }
    }
    fill_playback(s, t);
    resubmit(s, x);
}

/* ---- MIDI ------------------------------------------------------------- */

static void LIBUSB_CALL midi_in_done(struct libusb_transfer *t)
{
    xfer *x = t->user_data;
    mbox_stream *s = x->s;
    if (!completed(s, x))
        return;
    if (t->status == LIBUSB_TRANSFER_COMPLETED && t->actual_length > 0 && s->cb.midi_in) {
        uint8_t bytes[256];
        size_t n = mbox_midi_decode(t->buffer, (size_t)t->actual_length, bytes, sizeof bytes);
        if (n)
            s->cb.midi_in(s->cb.ctx, bytes, n);
    }
    resubmit(s, x);
}

/* midi_lock held. */
static void midi_kick(mbox_stream *s)
{
    if (s->midi_out.in_flight || s->midi_q_len == 0 || atomic_load(&s->stopping))
        return;
    size_t n = s->midi_q_len < s->midi_out_max ? s->midi_q_len : s->midi_out_max;
    n -= n % MBOX_MIDI_PACKET_SIZE;
    memcpy(s->midi_out.t->buffer, s->midi_q, n);
    s->midi_out.t->length = (int)n;
    s->midi_in_flight_len = n;
    if (submit(s, &s->midi_out) != 0)
        s->midi_in_flight_len = 0;
}

static void LIBUSB_CALL midi_out_done(struct libusb_transfer *t)
{
    xfer *x = t->user_data;
    mbox_stream *s = x->s;
    pthread_mutex_lock(&s->midi_lock);
    bool again = completed(s, x);
    size_t sent = s->midi_in_flight_len;
    memmove(s->midi_q, s->midi_q + sent, s->midi_q_len - sent);
    s->midi_q_len -= sent;
    s->midi_in_flight_len = 0;
    if (again)
        midi_kick(s);
    pthread_mutex_unlock(&s->midi_lock);
}

size_t mbox_stream_midi_send(mbox_stream *s, const uint8_t *bytes, size_t n)
{
    if (!s->midi_out.t)
        return 0;
    pthread_mutex_lock(&s->midi_lock);
    size_t consumed = 0;
    s->midi_q_len += mbox_midi_encode(&s->midi_parser, bytes, n, 0, s->midi_q + s->midi_q_len,
                                      MIDI_QUEUE - s->midi_q_len, &consumed);
    midi_kick(s);
    pthread_mutex_unlock(&s->midi_lock);
    return consumed;
}

/* ---- setup ------------------------------------------------------------ */

static xfer *alloc_xfers(uint32_t n)
{
    return calloc(n, sizeof(xfer));
}

static xfer *xfer_at(xfer *base, uint32_t i)
{
    return &base[i];
}

static int setup_iso(mbox_stream *s, xfer *x, uint8_t ep, unsigned max_packet, libusb_transfer_cb_fn cb)
{
    int packets = (int)s->cfg.packets_per_transfer;
    struct libusb_transfer *t = libusb_alloc_transfer(packets);
    unsigned char *buf = calloc((size_t)packets, max_packet);
    if (!t || !buf) {
        libusb_free_transfer(t);
        free(buf);
        return LIBUSB_ERROR_NO_MEM;
    }
    x->s = s;
    libusb_fill_iso_transfer(t, s->h, ep, buf, packets * (int)max_packet, packets, cb, x, 0);
    libusb_set_iso_packet_lengths(t, max_packet);
    t->flags = LIBUSB_TRANSFER_FREE_BUFFER;
    x->t = t;
    return 0;
}

/* Finds an endpoint's descriptor on the model's MIDI interface. */
static int midi_endpoint(mbox_stream *s, uint8_t addr, uint8_t *type, unsigned *max_packet)
{
    struct libusb_config_descriptor *cfg;
    int r = libusb_get_active_config_descriptor(libusb_get_device(s->h), &cfg);
    if (r != 0)
        return r;
    r = MBOX_ERR_BAD_DESCRIPTOR;
    for (int i = 0; i < cfg->bNumInterfaces && r != 0; i++) {
        const struct libusb_interface_descriptor *id = &cfg->interface[i].altsetting[0];
        if (id->bInterfaceNumber != s->m->midi_iface)
            continue;
        for (int e = 0; e < id->bNumEndpoints; e++) {
            if (id->endpoint[e].bEndpointAddress == addr) {
                *type = id->endpoint[e].bmAttributes & 3;
                *max_packet = id->endpoint[e].wMaxPacketSize;
                r = 0;
            }
        }
    }
    libusb_free_config_descriptor(cfg);
    return r;
}

static int setup_midi(mbox_stream *s, xfer *x, uint8_t ep, libusb_transfer_cb_fn cb)
{
    uint8_t type;
    unsigned maxp;
    int r = midi_endpoint(s, ep, &type, &maxp);
    if (r != 0)
        return r;
    struct libusb_transfer *t = libusb_alloc_transfer(0);
    unsigned char *buf = calloc(1, maxp);
    if (!t || !buf) {
        libusb_free_transfer(t);
        free(buf);
        return LIBUSB_ERROR_NO_MEM;
    }
    x->s = s;
    if (type == LIBUSB_TRANSFER_TYPE_INTERRUPT)
        libusb_fill_interrupt_transfer(t, s->h, ep, buf, (int)maxp, cb, x, 0);
    else
        libusb_fill_bulk_transfer(t, s->h, ep, buf, (int)maxp, cb, x, 0);
    t->flags = LIBUSB_TRANSFER_FREE_BUFFER;
    x->t = t;
    if (!(ep & LIBUSB_ENDPOINT_IN))
        s->midi_out_max = maxp;
    return 0;
}

static void free_all(mbox_stream *s)
{
    for (uint32_t i = 0; i < s->cfg.transfers; i++) {
        if (s->cap)
            libusb_free_transfer(xfer_at(s->cap, i)->t);
        if (s->play)
            libusb_free_transfer(xfer_at(s->play, i)->t);
    }
    libusb_free_transfer(s->midi_in.t);
    libusb_free_transfer(s->midi_out.t);
    free(s->cap);
    free(s->play);
    free(s->scratch);
    pthread_mutex_destroy(&s->midi_lock);
    free(s);
}

int mbox_stream_start(mbox_device *d, uint32_t rate, const mbox_stream_config *cfg,
                      const mbox_stream_callbacks *cb, mbox_stream **out)
{
    mbox_stream *s = calloc(1, sizeof *s);
    if (!s)
        return LIBUSB_ERROR_NO_MEM;
    s->dev = d;
    s->h = mbox_device_handle(d);
    s->m = mbox_device_model(d);
    s->cb = *cb;
    s->cfg.transfers = cfg && cfg->transfers ? cfg->transfers : 3;
    s->cfg.packets_per_transfer = cfg && cfg->packets_per_transfer ? cfg->packets_per_transfer : 8;
    s->rate = rate;
    s->ticks_per_packet = mbox_host_ticks_per_second() / PACKETS_PER_SECOND;
    mbox_pacer_init(&s->out_pacer, rate);
    mbox_pacer_init(&s->gap_pacer, rate);
    pthread_mutex_init(&s->midi_lock, NULL);

    unsigned bps = mbox_pcm_bytes_per_sample(s->m->format);
    s->in_frame_bytes = bps * s->m->capture.channels;
    s->out_frame_bytes = bps * s->m->playback.channels;

    libusb_device *dev = libusb_get_device(s->h);
    int r = 0;
    unsigned max_ch = s->m->capture.channels > s->m->playback.channels ? s->m->capture.channels
                                                                       : s->m->playback.channels;
    if (s->m->capture.endpoint) {
        int mp = libusb_get_max_iso_packet_size(dev, s->m->capture.endpoint);
        if (mp <= 0) {
            r = mp < 0 ? mp : MBOX_ERR_BAD_DESCRIPTOR;
            goto fail;
        }
        s->in_max_packet = (unsigned)mp;
    }
    if (s->m->playback.endpoint) {
        int mp = libusb_get_max_iso_packet_size(dev, s->m->playback.endpoint);
        if (mp <= 0) {
            r = mp < 0 ? mp : MBOX_ERR_BAD_DESCRIPTOR;
            goto fail;
        }
        s->out_max_packet = (unsigned)mp;
    }
    unsigned max_packet = s->in_max_packet > s->out_max_packet ? s->in_max_packet : s->out_max_packet;
    unsigned min_frame = s->in_frame_bytes && s->in_frame_bytes < s->out_frame_bytes ? s->in_frame_bytes
                                                                                     : s->out_frame_bytes;
    if (!min_frame)
        min_frame = s->in_frame_bytes;
    s->scratch = calloc((size_t)(max_packet / min_frame + 1) * max_ch, sizeof(float));
    if (!s->scratch) {
        r = LIBUSB_ERROR_NO_MEM;
        goto fail;
    }

    if (s->m->capture.endpoint) {
        if (!(s->cap = alloc_xfers(s->cfg.transfers))) {
            r = LIBUSB_ERROR_NO_MEM;
            goto fail;
        }
        for (uint32_t i = 0; i < s->cfg.transfers; i++)
            if ((r = setup_iso(s, xfer_at(s->cap, i), s->m->capture.endpoint, s->in_max_packet, capture_done)))
                goto fail;
    }
    if (s->m->playback.endpoint) {
        if (!(s->play = alloc_xfers(s->cfg.transfers))) {
            r = LIBUSB_ERROR_NO_MEM;
            goto fail;
        }
        for (uint32_t i = 0; i < s->cfg.transfers; i++)
            if ((r = setup_iso(s, xfer_at(s->play, i), s->m->playback.endpoint, s->out_max_packet, playback_done)))
                goto fail;
    }
    if (s->m->midi_iface != 0xff) {
        /* MIDI is optional: a failure here only disables it. */
        if (setup_midi(s, &s->midi_in, s->m->midi_in_ep, midi_in_done) != 0 ||
            setup_midi(s, &s->midi_out, s->m->midi_out_ep, midi_out_done) != 0) {
            libusb_free_transfer(s->midi_in.t);
            libusb_free_transfer(s->midi_out.t);
            s->midi_in.t = s->midi_out.t = NULL;
        }
    }

    for (uint32_t i = 0; s->cap && i < s->cfg.transfers; i++)
        if ((r = submit(s, xfer_at(s->cap, i))))
            goto fail_running;
    for (uint32_t i = 0; s->play && i < s->cfg.transfers; i++) {
        fill_playback(s, xfer_at(s->play, i)->t);
        if ((r = submit(s, xfer_at(s->play, i))))
            goto fail_running;
    }
    if (s->midi_in.t && (r = submit(s, &s->midi_in)))
        goto fail_running;

    *out = s;
    return 0;

fail_running:
    mbox_stream_stop(s);
    return r;
fail:
    free_all(s);
    return r;
}

void mbox_stream_stop(mbox_stream *s)
{
    atomic_store(&s->stopping, true);
    for (uint32_t i = 0; i < s->cfg.transfers; i++) {
        if (s->cap && xfer_at(s->cap, i)->in_flight)
            libusb_cancel_transfer(xfer_at(s->cap, i)->t);
        if (s->play && xfer_at(s->play, i)->in_flight)
            libusb_cancel_transfer(xfer_at(s->play, i)->t);
    }
    pthread_mutex_lock(&s->midi_lock);
    if (s->midi_in.in_flight)
        libusb_cancel_transfer(s->midi_in.t);
    if (s->midi_out.in_flight)
        libusb_cancel_transfer(s->midi_out.t);
    pthread_mutex_unlock(&s->midi_lock);

    libusb_context *ctx = mbox_device_context(s->dev);
    struct timeval tv = {0, 100000};
    for (int waited = 0; atomic_load(&s->in_flight) > 0 && waited < STOP_TIMEOUT_MS; waited += 100)
        libusb_handle_events_timeout_completed(ctx, &tv, NULL);
    free_all(s);
}

uint32_t mbox_stream_output_latency(const mbox_stream *s)
{
    if (!s->m->playback.endpoint)
        return 0;
    return (uint32_t)((uint64_t)s->rate * s->cfg.transfers * s->cfg.packets_per_transfer / PACKETS_PER_SECOND);
}

uint32_t mbox_stream_input_latency(const mbox_stream *s)
{
    if (!s->m->capture.endpoint)
        return 0;
    /* A transfer is delivered only once all its packets have arrived. */
    return (uint32_t)((uint64_t)s->rate * s->cfg.packets_per_transfer / PACKETS_PER_SECOND);
}
