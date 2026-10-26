/*
 * Model profiles for the Digidesign / Avid Mbox 2 family.
 *
 * Every device-specific constant lives here so that adding a model
 * (e.g. once the Mbox 2 Mini's USB protocol has been captured) only
 * means adding a table entry, not touching the streaming engine.
 */
#ifndef MBOX_MODELS_H
#define MBOX_MODELS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MBOX_DIGIDESIGN_VID 0x0dba

typedef enum {
    MBOX_TRANSPORT_USB,
    MBOX_TRANSPORT_FIREWIRE,
} mbox_transport;

typedef enum {
    /* Protocol known and implemented. */
    MBOX_SUPPORT_SUPPORTED,
    /* Profile is a hypothesis that still needs a hardware capture. */
    MBOX_SUPPORT_EXPERIMENTAL,
    /* Cannot be driven on current macOS (see notes). */
    MBOX_SUPPORT_UNSUPPORTED,
} mbox_support;

typedef enum {
    MBOX_BOOT_NONE,
    /* Poll vendor request 0x85 until the on-board firmware reports ready. */
    MBOX_BOOT_MBOX2,
} mbox_boot_kind;

typedef enum {
    MBOX_PCM_S24_3BE,
    MBOX_PCM_S24_3LE,
    MBOX_PCM_S16_LE,
} mbox_pcm_format;

typedef struct {
    uint8_t iface;
    uint8_t altsetting;
    uint8_t endpoint; /* 0 = direction not present on this model */
    uint8_t channels;
} mbox_stream_desc;

typedef struct {
    const char *id;   /* short CLI name: "mbox2", "mini", ... */
    const char *name; /* user-visible name */
    mbox_transport transport;
    mbox_support support;
    uint16_t vid;
    uint16_t pid; /* 0 = not yet known, must be given with --pid */
    mbox_boot_kind boot;
    /* wTotalLength of the configuration descriptor while the device is
     * still in its boot-loader state (0 = no boot-loader state). */
    uint16_t bootloader_config_len;
    mbox_pcm_format format;
    mbox_stream_desc playback;
    mbox_stream_desc capture;
    uint8_t midi_iface; /* 0xff = no MIDI */
    uint8_t midi_out_ep;
    uint8_t midi_in_ep;
    const uint32_t *rates;
    size_t n_rates;
    const char *notes;
} mbox_model;

size_t mbox_model_count(void);
const mbox_model *mbox_model_at(size_t index);
const mbox_model *mbox_model_by_id(const char *id);
/* Returns NULL when the pid is unknown; the caller may then fall back to
 * an explicitly selected experimental profile. */
const mbox_model *mbox_model_by_usb(uint16_t vid, uint16_t pid);

bool mbox_model_supports_rate(const mbox_model *m, uint32_t rate);
unsigned mbox_pcm_bytes_per_sample(mbox_pcm_format fmt);
const char *mbox_support_name(mbox_support s);

#ifdef __cplusplus
}
#endif

#endif
