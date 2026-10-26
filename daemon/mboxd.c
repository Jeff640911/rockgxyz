/*
 * mboxd: owns the Mbox over USB and bridges it to shared memory.
 *
 * Runs as a LaunchDaemon (root) on macOS. The Core Audio plug-in and the
 * CoreMIDI agent (mboxmidi) map the region through the XPC service in
 * transport.c; nothing else talks to the device.
 */
#include <errno.h>
#include <getopt.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "mbox/clock.h"
#include "mbox/device.h"
#include "mbox/hosttime.h"
#include "mbox/models.h"
#include "mbox/shm.h"
#include "mbox/stream.h"
#include "transport.h"

#define RING_FRAMES 16384 /* ~340 ms at 48 kHz */
#define MAX_CHANNELS 2
#define ZTS_PERIOD 512
#define RETRY_US 1000000
#define MIDI_POLL_US 1000

typedef struct {
    mbox_shm_header *h;
    mbox_tl_ring in_ring, out_ring;
    mbox_clock clock;
    int64_t device_time; /* frames clocked by the device so far */
    int64_t cap_cursor;
    int64_t out_cursor;
    int64_t next_anchor;
    uint64_t generation;
    pthread_mutex_t stream_lock; /* guards `stream` against the MIDI pump */
    mbox_stream *stream;
    atomic_bool failed;
} bridge;

static atomic_bool running = true;
static int verbose;

static void on_signal(int sig)
{
    (void)sig;
    atomic_store(&running, false);
}

#define LOG(...) fprintf(stderr, "mboxd: " __VA_ARGS__)
#define DEBUG(...)               \
    do {                         \
        if (verbose)             \
            LOG(__VA_ARGS__);    \
    } while (0)

/* ---- stream callbacks (USB event thread) ------------------------------ */

static void on_clock(void *ctx, uint32_t frames, uint64_t host_time)
{
    bridge *b = ctx;
    b->device_time += frames;
    mbox_clock_observe(&b->clock, b->device_time, host_time);
    while (b->next_anchor <= b->device_time) {
        mbox_anchor a = {
            .sample_time = b->next_anchor,
            .host_time = mbox_clock_host_time(&b->clock, b->next_anchor),
            .generation = b->generation,
        };
        mbox_shm_publish_anchor(b->h, &a);
        b->next_anchor += ZTS_PERIOD;
    }
}

static void on_capture(void *ctx, const float *frames, uint32_t n)
{
    bridge *b = ctx;
    mbox_tl_write(&b->in_ring, b->cap_cursor, frames, n);
    b->cap_cursor += n;
}

static void on_playback(void *ctx, float *frames, uint32_t n)
{
    bridge *b = ctx;
    mbox_tl_consume(&b->out_ring, b->out_cursor, frames, n);
    b->out_cursor += n;
    atomic_store_explicit(&b->h->out_cursor, b->out_cursor, memory_order_release);
}

static void on_midi_in(void *ctx, const uint8_t *bytes, size_t n)
{
    bridge *b = ctx;
    if (mbox_byte_ring_write(&b->h->midi_in, bytes, n) < n)
        DEBUG("MIDI input overflow, %zu bytes dropped\n", n);
}

static void on_fatal(void *ctx, int err)
{
    bridge *b = ctx;
    if (!atomic_exchange(&b->failed, true))
        LOG("stream stopped: %s\n", mbox_strerror(err));
}

/* ---- threads ---------------------------------------------------------- */

static void *usb_events(void *arg)
{
    libusb_context *ctx = arg;
    struct timeval tv = {0, 100000};
    while (atomic_load(&running))
        libusb_handle_events_timeout_completed(ctx, &tv, NULL);
    return NULL;
}

/* Moves MIDI from the agent's FIFO to the device. */
static void *midi_out_pump(void *arg)
{
    bridge *b = arg;
    uint8_t buf[256];
    while (atomic_load(&running)) {
        pthread_mutex_lock(&b->stream_lock);
        if (b->stream && !atomic_load(&b->failed)) {
            size_t n = mbox_byte_ring_read(&b->h->midi_out, buf, sizeof buf);
            if (n)
                mbox_stream_midi_send(b->stream, buf, n);
        }
        pthread_mutex_unlock(&b->stream_lock);
        usleep(MIDI_POLL_US);
    }
    return NULL;
}

/* ---- device lifecycle ------------------------------------------------- */

typedef struct {
    const mbox_model *model; /* NULL = autodetect */
    uint16_t pid;
    uint32_t rate;
    mbox_stream_config stream;
} options;

static void publish_device(bridge *b, const mbox_model *m, uint32_t rate)
{
    mbox_shm_header *h = b->h;
    h->sample_rate = rate;
    h->in_channels = m->capture.channels;
    h->out_channels = m->playback.channels;
    h->out_latency = mbox_stream_output_latency(b->stream);
    h->in_latency = mbox_stream_input_latency(b->stream);
    snprintf(h->model_id, sizeof h->model_id, "%s", m->id);
    snprintf(h->model_name, sizeof h->model_name, "%s", m->name);
}

/* Runs one device session: open, boot, stream until it fails. Returns
 * false when no device was available. */
static bool run_session(libusb_context *ctx, bridge *b, const options *o)
{
    mbox_device *d = NULL;
    int r = mbox_device_open(ctx, o->model, o->pid, &d);
    if (r != 0) {
        DEBUG("open: %s\n", mbox_strerror(r));
        return false;
    }
    const mbox_model *m = mbox_device_model(d);
    if (m->support != MBOX_SUPPORT_SUPPORTED)
        LOG("%s support is %s: %s\n", m->name, mbox_support_name(m->support), m->notes);

    r = mbox_device_boot(d);
    if (r == MBOX_REOPEN) {
        LOG("%s re-enumerated after boot, reopening\n", m->name);
        mbox_device_close(d);
        return true;
    }
    if (r != 0) {
        LOG("boot: %s\n", mbox_strerror(r));
        goto out;
    }

    uint32_t rate = o->rate ? o->rate : m->rates[m->n_rates - 1];
    if ((r = mbox_device_claim(d)) != 0) {
        LOG("claim: %s\n", mbox_strerror(r));
        goto out;
    }
    if ((r = mbox_device_set_rate(d, rate)) != 0) {
        LOG("set rate %u: %s\n", rate, mbox_strerror(r));
        goto out;
    }

    /* Continue the timeline where the previous session left off so the
     * plug-in's sample times keep increasing. */
    b->generation++;
    b->cap_cursor = b->device_time;
    b->out_cursor = b->device_time;
    b->next_anchor = (b->device_time / ZTS_PERIOD + 1) * ZTS_PERIOD;
    mbox_clock_init(&b->clock, rate, mbox_host_ticks_per_second());
    mbox_tl_clear(&b->in_ring, 0, b->in_ring.capacity_frames);
    mbox_tl_clear(&b->out_ring, 0, b->out_ring.capacity_frames);
    atomic_store(&b->failed, false);

    mbox_stream_callbacks cb = {
        .ctx = b,
        .clock = on_clock,
        .capture = on_capture,
        .playback = on_playback,
        .midi_in = on_midi_in,
        .fatal = on_fatal,
    };
    mbox_stream *s;
    if ((r = mbox_stream_start(d, rate, &o->stream, &cb, &s)) != 0) {
        LOG("stream: %s\n", mbox_strerror(r));
        goto out;
    }
    pthread_mutex_lock(&b->stream_lock);
    b->stream = s;
    pthread_mutex_unlock(&b->stream_lock);
    publish_device(b, m, rate);
    atomic_store(&b->h->state, MBOX_STATE_RUNNING);
    LOG("%s running at %u Hz (out latency %u, in latency %u frames)\n", m->name, rate, b->h->out_latency,
        b->h->in_latency);

    while (atomic_load(&running) && !atomic_load(&b->failed))
        usleep(100000);

    atomic_store(&b->h->state, MBOX_STATE_ABSENT);
    pthread_mutex_lock(&b->stream_lock);
    b->stream = NULL;
    pthread_mutex_unlock(&b->stream_lock);
    mbox_stream_stop(s);
    LOG("%s stopped\n", m->name);
out:
    mbox_device_close(d);
    return true;
}

static void usage(FILE *f)
{
    fprintf(f,
            "usage: mboxd [-v] [--model ID] [--pid 0xNNNN] [--rate HZ]\n"
            "             [--transfers N] [--packets N]\n"
            "  --model  force a profile (mbox2, mini, micro); default autodetect\n"
            "  --pid    USB product id, required for profiles with an unknown pid\n");
}

int main(int argc, char **argv)
{
    options o = {0};
    static const struct option longopts[] = {
        {"model", required_argument, NULL, 'm'}, {"pid", required_argument, NULL, 'p'},
        {"rate", required_argument, NULL, 'r'},  {"transfers", required_argument, NULL, 't'},
        {"packets", required_argument, NULL, 'k'}, {"verbose", no_argument, NULL, 'v'},
        {"help", no_argument, NULL, 'h'},        {NULL, 0, NULL, 0},
    };
    int c;
    while ((c = getopt_long(argc, argv, "vh", longopts, NULL)) != -1) {
        switch (c) {
        case 'm':
            if (!(o.model = mbox_model_by_id(optarg))) {
                LOG("unknown model '%s'\n", optarg);
                return 2;
            }
            break;
        case 'p':
            o.pid = (uint16_t)strtoul(optarg, NULL, 0);
            break;
        case 'r':
            o.rate = (uint32_t)strtoul(optarg, NULL, 0);
            break;
        case 't':
            o.stream.transfers = (uint32_t)strtoul(optarg, NULL, 0);
            break;
        case 'k':
            o.stream.packets_per_transfer = (uint32_t)strtoul(optarg, NULL, 0);
            break;
        case 'v':
            verbose = 1;
            break;
        case 'h':
            usage(stdout);
            return 0;
        default:
            usage(stderr);
            return 2;
        }
    }
    if (o.model && o.model->transport != MBOX_TRANSPORT_USB) {
        LOG("%s: %s\n", o.model->name, o.model->notes);
        return 1;
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    libusb_context *ctx;
    int r = libusb_init(&ctx);
    if (r != 0) {
        LOG("libusb_init: %s\n", libusb_strerror(r));
        return 1;
    }

    size_t size = mbox_shm_size(RING_FRAMES, MAX_CHANNELS, MAX_CHANNELS);
    void *base = mboxd_transport_start(size);
    if (!base) {
        LOG("cannot create shared memory\n");
        return 1;
    }
    mbox_shm_init(base, RING_FRAMES, MAX_CHANNELS, MAX_CHANNELS, ZTS_PERIOD);

    bridge b = {.h = base};
    pthread_mutex_init(&b.stream_lock, NULL);
    b.in_ring = mbox_shm_input_ring(base);
    b.out_ring = mbox_shm_output_ring(base);

    pthread_t events, midi;
    pthread_create(&events, NULL, usb_events, ctx);
    pthread_create(&midi, NULL, midi_out_pump, &b);

    LOG("waiting for a device\n");
    while (atomic_load(&running)) {
        if (!run_session(ctx, &b, &o) || atomic_load(&running))
            usleep(RETRY_US);
    }

    pthread_join(midi, NULL);
    pthread_join(events, NULL);
    mboxd_transport_stop();
    libusb_exit(ctx);
    return 0;
}
