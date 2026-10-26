#include "mbox/device.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CTRL_TIMEOUT_MS 1000

/* USB Audio Class 1 requests used by the rate-select sequence. */
#define UAC_SET_CUR 0x01
#define UAC_GET_CUR 0x81
#define UAC_SAMPLING_FREQ_CONTROL 0x0100
#define RT_CLASS_EP_OUT (LIBUSB_REQUEST_TYPE_CLASS | LIBUSB_RECIPIENT_ENDPOINT | LIBUSB_ENDPOINT_OUT)
#define RT_CLASS_EP_IN (LIBUSB_REQUEST_TYPE_CLASS | LIBUSB_RECIPIENT_ENDPOINT | LIBUSB_ENDPOINT_IN)

/* Mbox 2 boot loader, see docs/PROTOCOL-mbox2.md section 2. */
#define MBOX2_BOOT_REQUEST 0x85
#define MBOX2_BOOT_LOADING 0x01
#define MBOX2_BOOT_READY 0x02
#define MBOX2_BOOT_POLLS 10
#define MBOX2_BOOT_POLL_US 500000

struct mbox_device {
    libusb_context *ctx;
    libusb_device_handle *handle;
    const mbox_model *model;
    uint16_t pid;
    uint32_t claimed; /* bitmask of claimed interface numbers */
};

int mbox_find(libusb_context *ctx, mbox_found *out, int cap)
{
    libusb_device **list;
    ssize_t n = libusb_get_device_list(ctx, &list);
    if (n < 0)
        return (int)n;
    int found = 0;
    for (ssize_t i = 0; i < n; i++) {
        struct libusb_device_descriptor dd;
        if (libusb_get_device_descriptor(list[i], &dd) != 0 || dd.idVendor != MBOX_DIGIDESIGN_VID)
            continue;
        if (found < cap) {
            out[found].vid = dd.idVendor;
            out[found].pid = dd.idProduct;
            out[found].bus = libusb_get_bus_number(list[i]);
            out[found].address = libusb_get_device_address(list[i]);
            out[found].model = mbox_model_by_usb(dd.idVendor, dd.idProduct);
        }
        found++;
    }
    libusb_free_device_list(list, 1);
    return found;
}

int mbox_device_open(libusb_context *ctx, const mbox_model *model, uint16_t pid, mbox_device **out)
{
    if (model && model->transport != MBOX_TRANSPORT_USB)
        return MBOX_ERR_UNSUPPORTED;
    if (model && pid == 0)
        pid = model->pid;
    if (model && pid == 0)
        return MBOX_ERR_NOT_FOUND; /* unknown pid must be provided */

    libusb_device **list;
    ssize_t n = libusb_get_device_list(ctx, &list);
    if (n < 0)
        return (int)n;

    int err = MBOX_ERR_NOT_FOUND;
    for (ssize_t i = 0; i < n; i++) {
        struct libusb_device_descriptor dd;
        if (libusb_get_device_descriptor(list[i], &dd) != 0 || dd.idVendor != MBOX_DIGIDESIGN_VID)
            continue;
        const mbox_model *m = model;
        if (m) {
            if (dd.idProduct != pid)
                continue;
        } else {
            if (pid && dd.idProduct != pid)
                continue;
            m = mbox_model_by_usb(dd.idVendor, dd.idProduct);
            if (!m)
                continue;
        }

        mbox_device *d = calloc(1, sizeof *d);
        if (!d) {
            err = LIBUSB_ERROR_NO_MEM;
            break;
        }
        err = libusb_open(list[i], &d->handle);
        if (err != 0) {
            free(d);
            break;
        }
        d->ctx = ctx;
        d->model = m;
        d->pid = dd.idProduct;
        /* Lets libusb detach a kernel driver (Linux) when we claim. */
        libusb_set_auto_detach_kernel_driver(d->handle, 1);
        *out = d;
        err = 0;
        break;
    }
    libusb_free_device_list(list, 1);
    return err;
}

void mbox_device_close(mbox_device *d)
{
    if (!d)
        return;
    for (int i = 0; i < 32; i++)
        if (d->claimed & (1u << i))
            libusb_release_interface(d->handle, i);
    libusb_close(d->handle);
    free(d);
}

const mbox_model *mbox_device_model(const mbox_device *d)
{
    return d->model;
}

libusb_device_handle *mbox_device_handle(mbox_device *d)
{
    return d->handle;
}

libusb_context *mbox_device_context(mbox_device *d)
{
    return d->ctx;
}

/* wTotalLength as reported by the device right now, bypassing any
 * descriptor cache in the OS. */
static int live_config_length(mbox_device *d)
{
    uint8_t buf[LIBUSB_DT_CONFIG_SIZE];
    int r = libusb_get_descriptor(d->handle, LIBUSB_DT_CONFIG, 0, buf, sizeof buf);
    if (r < 0)
        return r;
    if (r < 4)
        return MBOX_ERR_BAD_DESCRIPTOR;
    return buf[2] | buf[3] << 8;
}

static int cached_interface_count(mbox_device *d)
{
    struct libusb_config_descriptor *cfg;
    int r = libusb_get_active_config_descriptor(libusb_get_device(d->handle), &cfg);
    if (r != 0)
        return r;
    int n = cfg->bNumInterfaces;
    libusb_free_config_descriptor(cfg);
    return n;
}

static int highest_interface_needed(const mbox_model *m)
{
    int hi = 0;
    if (m->playback.endpoint && m->playback.iface > hi)
        hi = m->playback.iface;
    if (m->capture.endpoint && m->capture.iface > hi)
        hi = m->capture.iface;
    if (m->midi_iface != 0xff && m->midi_iface > hi)
        hi = m->midi_iface;
    return hi;
}

static int boot_mbox2(mbox_device *d)
{
    int len = live_config_length(d);
    if (len < 0)
        return len;
    if (d->model->bootloader_config_len && len != d->model->bootloader_config_len) {
        /* Not the boot-loader descriptor: assume the firmware already runs
         * (e.g. the daemon restarted without a power cycle). */
        if (cached_interface_count(d) > highest_interface_needed(d->model))
            return 0;
        fprintf(stderr, "mbox: unexpected boot-loader config length %d (want %u)\n", len,
                d->model->bootloader_config_len);
        return MBOX_ERR_BAD_DESCRIPTOR;
    }
    if (!d->model->bootloader_config_len)
        fprintf(stderr, "mbox: experimental boot, config length is %d\n", len);

    uint8_t resp[0x12] = {MBOX2_BOOT_LOADING};
    for (int i = 0; i < MBOX2_BOOT_POLLS; i++) {
        usleep(MBOX2_BOOT_POLL_US);
        int r = libusb_control_transfer(d->handle,
                                        LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_DEVICE | LIBUSB_ENDPOINT_IN,
                                        MBOX2_BOOT_REQUEST, 0x0001, 0x0000, resp, sizeof resp, CTRL_TIMEOUT_MS);
        if (r >= 1 && resp[0] == MBOX2_BOOT_READY)
            break;
    }
    if (resp[0] != MBOX2_BOOT_READY)
        return MBOX_ERR_BOOT_TIMEOUT;

    /* The firmware now presents a larger configuration. Linux re-reads it
     * and re-selects the configuration; do the same. */
    int cfg = 1;
    libusb_get_configuration(d->handle, &cfg);
    int r = libusb_set_configuration(d->handle, cfg);
    if (r != 0 && r != LIBUSB_ERROR_BUSY)
        return r;

    if (cached_interface_count(d) > highest_interface_needed(d->model))
        return 0;

    /* The OS still caches the boot-loader descriptors (expected on macOS).
     * Force re-enumeration; libusb reports NOT_FOUND when the descriptors
     * changed, after which the caller must reopen. */
    r = libusb_reset_device(d->handle);
    if (r == LIBUSB_ERROR_NOT_FOUND || r == 0)
        return MBOX_REOPEN;
    return r;
}

int mbox_device_boot(mbox_device *d)
{
    switch (d->model->boot) {
    case MBOX_BOOT_NONE:
        return 0;
    case MBOX_BOOT_MBOX2:
        return boot_mbox2(d);
    }
    return MBOX_ERR_UNSUPPORTED;
}

static int claim(mbox_device *d, uint8_t iface, uint8_t alt)
{
    int r = libusb_claim_interface(d->handle, iface);
    if (r != 0)
        return r;
    d->claimed |= 1u << iface;
    return libusb_set_interface_alt_setting(d->handle, iface, alt);
}

int mbox_device_claim(mbox_device *d)
{
    const mbox_model *m = d->model;
    int r;
    if (m->playback.endpoint && (r = claim(d, m->playback.iface, m->playback.altsetting)) != 0)
        return r;
    if (m->capture.endpoint && (r = claim(d, m->capture.iface, m->capture.altsetting)) != 0)
        return r;
    if (m->midi_iface != 0xff && (r = claim(d, m->midi_iface, 0)) != 0)
        return r;
    return 0;
}

int mbox_device_set_rate(mbox_device *d, uint32_t rate)
{
    const mbox_model *m = d->model;
    if (!mbox_model_supports_rate(m, rate))
        return MBOX_ERR_BAD_RATE;

    uint8_t want[3] = {(uint8_t)rate, (uint8_t)(rate >> 8), (uint8_t)(rate >> 16)};
    uint8_t got[3];
    int r;

    /* Sequence observed on the Mbox 2 (docs/PROTOCOL-mbox2.md section 3):
     * SET_CUR on the capture endpoint, then GET_CUR on 0x85, 0x86, 0x03.
     * The GETs are part of the magic; their results are only logged. */
    uint8_t set_ep = m->capture.endpoint ? m->capture.endpoint : m->playback.endpoint;
    r = libusb_control_transfer(d->handle, RT_CLASS_EP_OUT, UAC_SET_CUR, UAC_SAMPLING_FREQ_CONTROL, set_ep, want,
                                sizeof want, CTRL_TIMEOUT_MS);
    if (r < 0)
        return r;

    const uint8_t get_eps[] = {0x85, 0x86, 0x03};
    for (size_t i = 0; i < sizeof get_eps; i++) {
        r = libusb_control_transfer(d->handle, RT_CLASS_EP_IN, UAC_GET_CUR, UAC_SAMPLING_FREQ_CONTROL, get_eps[i],
                                    got, sizeof got, CTRL_TIMEOUT_MS);
        if (r == 3) {
            uint32_t v = got[0] | got[1] << 8 | (uint32_t)got[2] << 16;
            if (v != rate)
                fprintf(stderr, "mbox: endpoint 0x%02x reports %u Hz (asked %u)\n", get_eps[i], v, rate);
        } else if (r < 0 && m->support == MBOX_SUPPORT_SUPPORTED) {
            return r;
        }
    }
    return 0;
}

static const char *transfer_type_name(uint8_t attr)
{
    static const char *names[] = {"control", "isochronous", "bulk", "interrupt"};
    return names[attr & 3];
}

static const char *iso_sync_name(uint8_t attr)
{
    static const char *names[] = {"none", "async", "adaptive", "sync"};
    return names[(attr >> 2) & 3];
}

static void hexdump(FILE *f, const char *indent, const unsigned char *p, int n)
{
    for (int i = 0; i < n; i++)
        fprintf(f, "%s%02x%s", i % 16 == 0 ? indent : "", p[i], i % 16 == 15 || i == n - 1 ? "\n" : " ");
}

int mbox_dump_descriptors(libusb_device *dev, FILE *f)
{
    struct libusb_device_descriptor dd;
    int r = libusb_get_device_descriptor(dev, &dd);
    if (r != 0)
        return r;
    fprintf(f, "Device %04x:%04x  bcdUSB %x.%02x  class %u  configs %u  speed %d\n", dd.idVendor, dd.idProduct,
            dd.bcdUSB >> 8, dd.bcdUSB & 0xff, dd.bDeviceClass, dd.bNumConfigurations, libusb_get_device_speed(dev));

    for (uint8_t c = 0; c < dd.bNumConfigurations; c++) {
        struct libusb_config_descriptor *cfg;
        if ((r = libusb_get_config_descriptor(dev, c, &cfg)) != 0)
            return r;
        fprintf(f, "  Config %u: wTotalLength %u, %u interfaces, attr 0x%02x, %u mA\n", cfg->bConfigurationValue,
                cfg->wTotalLength, cfg->bNumInterfaces, cfg->bmAttributes, cfg->MaxPower * 2);
        if (cfg->extra_length)
            hexdump(f, "    extra: ", cfg->extra, cfg->extra_length);
        for (int i = 0; i < cfg->bNumInterfaces; i++) {
            const struct libusb_interface *itf = &cfg->interface[i];
            for (int a = 0; a < itf->num_altsetting; a++) {
                const struct libusb_interface_descriptor *id = &itf->altsetting[a];
                fprintf(f, "    Interface %u alt %u: class %u/%u/%u, %u endpoints\n", id->bInterfaceNumber,
                        id->bAlternateSetting, id->bInterfaceClass, id->bInterfaceSubClass,
                        id->bInterfaceProtocol, id->bNumEndpoints);
                if (id->extra_length)
                    hexdump(f, "      class-specific: ", id->extra, id->extra_length);
                for (int e = 0; e < id->bNumEndpoints; e++) {
                    const struct libusb_endpoint_descriptor *ep = &id->endpoint[e];
                    fprintf(f, "      EP 0x%02x %s %s, maxpacket %u, interval %u\n", ep->bEndpointAddress,
                            transfer_type_name(ep->bmAttributes),
                            (ep->bmAttributes & 3) == LIBUSB_TRANSFER_TYPE_ISOCHRONOUS ? iso_sync_name(ep->bmAttributes)
                                                                                      : "",
                            ep->wMaxPacketSize, ep->bInterval);
                    if (ep->extra_length)
                        hexdump(f, "        class-specific: ", ep->extra, ep->extra_length);
                }
            }
        }
        libusb_free_config_descriptor(cfg);
    }
    return 0;
}

const char *mbox_strerror(int err)
{
    switch (err) {
    case MBOX_ERR_NOT_FOUND:
        return "no matching device (unknown models need --model and --pid)";
    case MBOX_ERR_BOOT_TIMEOUT:
        return "firmware did not report ready";
    case MBOX_ERR_UNSUPPORTED:
        return "model cannot be driven over USB";
    case MBOX_ERR_BAD_DESCRIPTOR:
        return "unexpected USB descriptor";
    case MBOX_ERR_BAD_RATE:
        return "sample rate not supported by this model";
    case MBOX_REOPEN:
        return "device re-enumerated, reopen it";
    default:
        return libusb_strerror(err);
    }
}
