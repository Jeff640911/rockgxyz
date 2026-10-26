/*
 * mboxctl: bring-up and reverse-engineering tool.
 *
 *   mboxctl models                 list known profiles and their status
 *   mboxctl list                   list connected Digidesign USB devices
 *   mboxctl probe [--pid P]        dump every descriptor (send this in bug reports)
 *   mboxctl boot  [opts]           run the boot sequence and dump the result
 *   mboxctl tone  [opts] [secs]    play a 1 kHz tone and report input levels
 *   mboxctl midi  [opts] [secs]    print incoming MIDI bytes
 *
 * opts: --model ID  --pid 0xNNNN  --rate HZ
 */
#include <getopt.h>
#include <math.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "mbox/device.h"
#include "mbox/models.h"
#include "mbox/stream.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

typedef struct {
    const mbox_model *model;
    uint16_t pid;
    uint32_t rate;
} options;

static atomic_bool stop_requested;

static void on_signal(int sig)
{
    (void)sig;
    atomic_store(&stop_requested, true);
}

static int cmd_models(void)
{
    printf("%-6s %-14s %-9s %-12s %-6s %s\n", "id", "name", "bus", "status", "pid", "notes");
    for (size_t i = 0; i < mbox_model_count(); i++) {
        const mbox_model *m = mbox_model_at(i);
        char pid[8] = "?";
        if (m->pid)
            snprintf(pid, sizeof pid, "%04x", m->pid);
        printf("%-6s %-14s %-9s %-12s %-6s %s\n", m->id, m->name,
               m->transport == MBOX_TRANSPORT_USB ? "USB" : "FireWire", mbox_support_name(m->support), pid,
               m->notes);
    }
    return 0;
}

static int cmd_list(libusb_context *ctx)
{
    mbox_found found[16];
    int n = mbox_find(ctx, found, 16);
    if (n < 0) {
        fprintf(stderr, "list: %s\n", mbox_strerror(n));
        return 1;
    }
    if (n == 0)
        printf("no Digidesign USB devices found\n");
    for (int i = 0; i < n && i < 16; i++)
        printf("bus %03u addr %03u  %04x:%04x  %s\n", found[i].bus, found[i].address, found[i].vid, found[i].pid,
               found[i].model ? found[i].model->name
                              : "unknown Digidesign product: run `mboxctl probe` and see docs/REVERSE-ENGINEERING.md");
    return 0;
}

static int cmd_probe(libusb_context *ctx, const options *o)
{
    libusb_device **list;
    ssize_t n = libusb_get_device_list(ctx, &list);
    if (n < 0)
        return 1;
    int dumped = 0;
    for (ssize_t i = 0; i < n; i++) {
        struct libusb_device_descriptor dd;
        if (libusb_get_device_descriptor(list[i], &dd) != 0 || dd.idVendor != MBOX_DIGIDESIGN_VID)
            continue;
        if (o->pid && dd.idProduct != o->pid)
            continue;

        mbox_dump_descriptors(list[i], stdout);
        libusb_device_handle *h;
        if (libusb_open(list[i], &h) == 0) {
            unsigned char s[128];
            if (dd.iManufacturer && libusb_get_string_descriptor_ascii(h, dd.iManufacturer, s, sizeof s) > 0)
                printf("  Manufacturer: %s\n", s);
            if (dd.iProduct && libusb_get_string_descriptor_ascii(h, dd.iProduct, s, sizeof s) > 0)
                printf("  Product: %s\n", s);
            /* Live config length, which differs from the cached one while
             * a boot-loader is swapping descriptors. */
            unsigned char cfg[9];
            if (libusb_get_descriptor(h, LIBUSB_DT_CONFIG, 0, cfg, sizeof cfg) >= 4)
                printf("  Live wTotalLength: %u\n", cfg[2] | cfg[3] << 8);
            libusb_close(h);
        } else {
            printf("  (could not open device for string descriptors; try sudo)\n");
        }
        printf("\n");
        dumped++;
    }
    libusb_free_device_list(list, 1);
    if (!dumped)
        printf("no matching Digidesign USB device\n");
    return dumped ? 0 : 1;
}

static int open_booted(libusb_context *ctx, const options *o, mbox_device **out)
{
    for (int attempt = 0; attempt < 3; attempt++) {
        mbox_device *d;
        int r = mbox_device_open(ctx, o->model, o->pid, &d);
        if (r != 0) {
            fprintf(stderr, "open: %s\n", mbox_strerror(r));
            return r;
        }
        const mbox_model *m = mbox_device_model(d);
        if (m->support != MBOX_SUPPORT_SUPPORTED)
            fprintf(stderr, "warning: %s is %s. %s\n", m->name, mbox_support_name(m->support), m->notes);
        printf("booting %s...\n", m->name);
        r = mbox_device_boot(d);
        if (r == 0) {
            *out = d;
            return 0;
        }
        mbox_device_close(d);
        if (r != MBOX_REOPEN) {
            fprintf(stderr, "boot: %s\n", mbox_strerror(r));
            return r;
        }
        printf("device re-enumerated, reopening\n");
        sleep(2);
    }
    return MBOX_ERR_BOOT_TIMEOUT;
}

static int cmd_boot(libusb_context *ctx, const options *o)
{
    mbox_device *d;
    if (open_booted(ctx, o, &d) != 0)
        return 1;
    printf("boot ok, descriptors now:\n");
    mbox_dump_descriptors(libusb_get_device(mbox_device_handle(d)), stdout);
    mbox_device_close(d);
    return 0;
}

typedef struct {
    double phase, step;
    double peak[8];
    uint64_t in_frames, out_frames;
    unsigned in_ch, out_ch;
    unsigned midi_bytes;
} tone_state;

static void tone_capture(void *ctx, const float *f, uint32_t n)
{
    tone_state *t = ctx;
    for (uint32_t i = 0; i < n; i++)
        for (unsigned c = 0; c < t->in_ch; c++) {
            double v = fabs(f[i * t->in_ch + c]);
            if (v > t->peak[c])
                t->peak[c] = v;
        }
    t->in_frames += n;
}

static void tone_playback(void *ctx, float *f, uint32_t n)
{
    tone_state *t = ctx;
    for (uint32_t i = 0; i < n; i++) {
        float v = (float)(0.25 * sin(t->phase));
        t->phase += t->step;
        if (t->phase > 2 * M_PI)
            t->phase -= 2 * M_PI;
        for (unsigned c = 0; c < t->out_ch; c++)
            f[i * t->out_ch + c] = v;
    }
    t->out_frames += n;
}

static void midi_print(void *ctx, const uint8_t *b, size_t n)
{
    tone_state *t = ctx;
    for (size_t i = 0; i < n; i++)
        printf("%02x ", b[i]);
    printf("\n");
    fflush(stdout);
    t->midi_bytes += (unsigned)n;
}

static int run_stream(libusb_context *ctx, const options *o, int seconds, bool midi_only)
{
    mbox_device *d;
    if (open_booted(ctx, o, &d) != 0)
        return 1;
    const mbox_model *m = mbox_device_model(d);
    uint32_t rate = o->rate ? o->rate : m->rates[m->n_rates - 1];
    int r;
    if ((r = mbox_device_claim(d)) != 0 || (r = mbox_device_set_rate(d, rate)) != 0) {
        fprintf(stderr, "setup: %s\n", mbox_strerror(r));
        mbox_device_close(d);
        return 1;
    }

    tone_state t = {.step = 2 * M_PI * 1000.0 / rate, .in_ch = m->capture.channels, .out_ch = m->playback.channels};
    mbox_stream_callbacks cb = {
        .ctx = &t,
        .capture = tone_capture,
        .playback = midi_only ? NULL : tone_playback,
        .midi_in = midi_print,
    };
    mbox_stream *s;
    if ((r = mbox_stream_start(d, rate, NULL, &cb, &s)) != 0) {
        fprintf(stderr, "stream: %s\n", mbox_strerror(r));
        mbox_device_close(d);
        return 1;
    }
    printf("%s at %u Hz for %d s, Ctrl-C to stop\n", m->name, rate, seconds);

    time_t end = time(NULL) + seconds;
    struct timeval tv = {0, 100000};
    time_t last = 0;
    while (!atomic_load(&stop_requested) && time(NULL) < end) {
        libusb_handle_events_timeout_completed(ctx, &tv, NULL);
        if (!midi_only && time(NULL) != last) {
            last = time(NULL);
            printf("out %8llu frames  in %8llu frames", (unsigned long long)t.out_frames,
                   (unsigned long long)t.in_frames);
            for (unsigned c = 0; c < t.in_ch; c++) {
                printf("  in%u peak %6.1f dBFS", c + 1, t.peak[c] > 0 ? 20 * log10(t.peak[c]) : -INFINITY);
                t.peak[c] = 0;
            }
            printf("\n");
        }
    }
    mbox_stream_stop(s);
    mbox_device_close(d);
    return 0;
}

static void usage(FILE *f)
{
    fprintf(f, "usage: mboxctl models | list | probe | boot | tone [secs] | midi [secs]\n"
               "       options: --model ID --pid 0xNNNN --rate HZ\n");
}

int main(int argc, char **argv)
{
    options o = {0};
    static const struct option longopts[] = {
        {"model", required_argument, NULL, 'm'},
        {"pid", required_argument, NULL, 'p'},
        {"rate", required_argument, NULL, 'r'},
        {"help", no_argument, NULL, 'h'},
        {NULL, 0, NULL, 0},
    };
    int c;
    while ((c = getopt_long(argc, argv, "h", longopts, NULL)) != -1) {
        switch (c) {
        case 'm':
            if (!(o.model = mbox_model_by_id(optarg))) {
                fprintf(stderr, "unknown model '%s' (see `mboxctl models`)\n", optarg);
                return 2;
            }
            break;
        case 'p':
            o.pid = (uint16_t)strtoul(optarg, NULL, 0);
            break;
        case 'r':
            o.rate = (uint32_t)strtoul(optarg, NULL, 0);
            break;
        case 'h':
            usage(stdout);
            return 0;
        default:
            usage(stderr);
            return 2;
        }
    }
    if (optind >= argc) {
        usage(stderr);
        return 2;
    }
    const char *cmd = argv[optind];
    int seconds = optind + 1 < argc ? atoi(argv[optind + 1]) : 10;

    if (strcmp(cmd, "models") == 0)
        return cmd_models();
    if (o.model && o.model->transport != MBOX_TRANSPORT_USB) {
        fprintf(stderr, "%s: %s\n", o.model->name, o.model->notes);
        return 1;
    }

    signal(SIGINT, on_signal);
    libusb_context *ctx;
    if (libusb_init(&ctx) != 0) {
        fprintf(stderr, "libusb_init failed\n");
        return 1;
    }
    int rc;
    if (strcmp(cmd, "list") == 0)
        rc = cmd_list(ctx);
    else if (strcmp(cmd, "probe") == 0)
        rc = cmd_probe(ctx, &o);
    else if (strcmp(cmd, "boot") == 0)
        rc = cmd_boot(ctx, &o);
    else if (strcmp(cmd, "tone") == 0)
        rc = run_stream(ctx, &o, seconds, false);
    else if (strcmp(cmd, "midi") == 0)
        rc = run_stream(ctx, &o, seconds, true);
    else {
        usage(stderr);
        rc = 2;
    }
    libusb_exit(ctx);
    return rc;
}
