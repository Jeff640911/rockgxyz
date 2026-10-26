#include "mbox/models.h"

#include <string.h>

static const uint32_t mbox2_rates[] = {48000};
/* 44.1 kHz is listed for the Mini/Micro on Avid's spec sheets; it is kept
 * experimental until the rate-select request has been captured. */
static const uint32_t gen2_rates[] = {44100, 48000};

static const mbox_model models[] = {
    {
        .id = "mbox2",
        .name = "Mbox 2",
        .transport = MBOX_TRANSPORT_USB,
        .support = MBOX_SUPPORT_SUPPORTED,
        .vid = MBOX_DIGIDESIGN_VID,
        .pid = 0x3000,
        .boot = MBOX_BOOT_MBOX2,
        .bootloader_config_len = 646,
        .format = MBOX_PCM_S24_3BE,
        .playback = {.iface = 2, .altsetting = 2, .endpoint = 0x03, .channels = 2},
        .capture = {.iface = 4, .altsetting = 2, .endpoint = 0x85, .channels = 2},
        .midi_iface = 6,
        .midi_out_ep = 0x02,
        .midi_in_ep = 0x81,
        .rates = mbox2_rates,
        .n_rates = sizeof mbox2_rates / sizeof mbox2_rates[0],
        .notes = "Protocol documented in docs/PROTOCOL-mbox2.md. 48 kHz / 24-bit only.",
    },
    {
        .id = "mini",
        .name = "Mbox 2 Mini",
        .transport = MBOX_TRANSPORT_USB,
        .support = MBOX_SUPPORT_EXPERIMENTAL,
        .vid = MBOX_DIGIDESIGN_VID,
        .pid = 0, /* unknown: run `mboxctl probe` and pass --pid */
        /* Hypothesis: same generation as the Mbox 2, same boot loader and
         * interface layout. Verify with docs/REVERSE-ENGINEERING.md. */
        .boot = MBOX_BOOT_MBOX2,
        .bootloader_config_len = 0, /* accept any length, log it */
        .format = MBOX_PCM_S24_3BE,
        .playback = {.iface = 2, .altsetting = 2, .endpoint = 0x03, .channels = 2},
        .capture = {.iface = 4, .altsetting = 2, .endpoint = 0x85, .channels = 2},
        .midi_iface = 0xff,
        .rates = gen2_rates,
        .n_rates = sizeof gen2_rates / sizeof gen2_rates[0],
        .notes = "No public protocol. Profile is a guess based on the Mbox 2; no MIDI.",
    },
    {
        .id = "micro",
        .name = "Mbox 2 Micro",
        .transport = MBOX_TRANSPORT_USB,
        .support = MBOX_SUPPORT_EXPERIMENTAL,
        .vid = MBOX_DIGIDESIGN_VID,
        .pid = 0,
        .boot = MBOX_BOOT_MBOX2,
        .bootloader_config_len = 0,
        .format = MBOX_PCM_S24_3BE,
        .playback = {.iface = 2, .altsetting = 2, .endpoint = 0x03, .channels = 2},
        .capture = {.iface = 0, .altsetting = 0, .endpoint = 0, .channels = 0},
        .midi_iface = 0xff,
        .rates = gen2_rates,
        .n_rates = sizeof gen2_rates / sizeof gen2_rates[0],
        .notes = "Output-only USB stick. Profile is a guess; no inputs, no MIDI.",
    },
    {
        .id = "pro",
        .name = "Mbox 2 Pro",
        .transport = MBOX_TRANSPORT_FIREWIRE,
        .support = MBOX_SUPPORT_UNSUPPORTED,
        .vid = MBOX_DIGIDESIGN_VID,
        .pid = 0,
        .boot = MBOX_BOOT_NONE,
        .format = MBOX_PCM_S24_3BE,
        .midi_iface = 0xff,
        .rates = NULL,
        .n_rates = 0,
        .notes = "FireWire only. No user-space FireWire audio path exists on current "
                 "macOS; see docs/MODELS.md.",
    },
};

size_t mbox_model_count(void)
{
    return sizeof models / sizeof models[0];
}

const mbox_model *mbox_model_at(size_t index)
{
    return index < mbox_model_count() ? &models[index] : NULL;
}

const mbox_model *mbox_model_by_id(const char *id)
{
    if (!id)
        return NULL;
    for (size_t i = 0; i < mbox_model_count(); i++)
        if (strcmp(models[i].id, id) == 0)
            return &models[i];
    return NULL;
}

const mbox_model *mbox_model_by_usb(uint16_t vid, uint16_t pid)
{
    for (size_t i = 0; i < mbox_model_count(); i++) {
        const mbox_model *m = &models[i];
        if (m->transport == MBOX_TRANSPORT_USB && m->pid != 0 && m->vid == vid && m->pid == pid)
            return m;
    }
    return NULL;
}

bool mbox_model_supports_rate(const mbox_model *m, uint32_t rate)
{
    for (size_t i = 0; i < m->n_rates; i++)
        if (m->rates[i] == rate)
            return true;
    return false;
}

unsigned mbox_pcm_bytes_per_sample(mbox_pcm_format fmt)
{
    switch (fmt) {
    case MBOX_PCM_S24_3BE:
    case MBOX_PCM_S24_3LE:
        return 3;
    case MBOX_PCM_S16_LE:
        return 2;
    }
    return 0;
}

const char *mbox_support_name(mbox_support s)
{
    switch (s) {
    case MBOX_SUPPORT_SUPPORTED:
        return "supported";
    case MBOX_SUPPORT_EXPERIMENTAL:
        return "experimental";
    case MBOX_SUPPORT_UNSUPPORTED:
        return "unsupported";
    }
    return "?";
}
