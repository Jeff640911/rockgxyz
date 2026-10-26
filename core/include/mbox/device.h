/* USB access to an Mbox 2 family device via libusb. */
#ifndef MBOX_DEVICE_H
#define MBOX_DEVICE_H

#include <stdint.h>
#include <stdio.h>

#include <libusb.h>

#include "mbox/models.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Return codes: 0 = ok, negative = libusb error or one of these. */
#define MBOX_ERR_NOT_FOUND (-1001)
#define MBOX_ERR_BOOT_TIMEOUT (-1002)
#define MBOX_ERR_UNSUPPORTED (-1003)
#define MBOX_ERR_BAD_DESCRIPTOR (-1004)
#define MBOX_ERR_BAD_RATE (-1005)
/* Boot succeeded but the device re-enumerated: close and open again. */
#define MBOX_REOPEN 1

typedef struct mbox_device mbox_device;

typedef struct {
    uint16_t vid;
    uint16_t pid;
    uint8_t bus;
    uint8_t address;
    const mbox_model *model; /* NULL = unknown Digidesign product */
} mbox_found;

/* Lists every device with the Digidesign vendor id; returns how many were
 * found (may exceed cap) or a negative error. */
int mbox_find(libusb_context *ctx, mbox_found *out, int cap);

/* Opens the first matching device. With `model` NULL the model is detected
 * from the product id. `pid` 0 means "the model's own pid"; it is required
 * for profiles whose pid is not known yet (Mini, Micro). */
int mbox_device_open(libusb_context *ctx, const mbox_model *model, uint16_t pid, mbox_device **out);
void mbox_device_close(mbox_device *d);

const mbox_model *mbox_device_model(const mbox_device *d);
libusb_device_handle *mbox_device_handle(mbox_device *d);
libusb_context *mbox_device_context(mbox_device *d);

/* Waits for the on-board firmware to come up. Returns 0, MBOX_REOPEN or
 * an error. A no-op for models without a boot loader. */
int mbox_device_boot(mbox_device *d);
/* Claims the audio/MIDI interfaces and selects their streaming altsettings. */
int mbox_device_claim(mbox_device *d);
int mbox_device_set_rate(mbox_device *d, uint32_t rate);

/* Human-readable dump of every descriptor, for reverse engineering. */
int mbox_dump_descriptors(libusb_device *dev, FILE *f);

const char *mbox_strerror(int err);

#ifdef __cplusplus
}
#endif

#endif
