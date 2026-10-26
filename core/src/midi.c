#include "mbox/midi.h"

#include <string.h>

void mbox_midi_parser_reset(mbox_midi_parser *p)
{
    memset(p, 0, sizeof *p);
}

static int emit(mbox_midi_parser *p, uint8_t cable, uint8_t *out)
{
    out[0] = p->len > 0 ? p->buf[0] : 0;
    out[1] = p->len > 1 ? p->buf[1] : 0;
    out[2] = p->len > 2 ? p->buf[2] : 0;
    out[3] = (uint8_t)((cable & 0x0f) << 4 | (p->len & 0x03));
    p->len = 0;
    return 1;
}

/* Number of bytes (status included) of a non-sysex message. */
static uint8_t message_length(uint8_t status)
{
    if (status < 0xf0) {
        uint8_t kind = status & 0xf0;
        return (kind == 0xc0 || kind == 0xd0) ? 2 : 3;
    }
    switch (status) {
    case 0xf1: /* MTC quarter frame */
    case 0xf3: /* song select */
        return 2;
    case 0xf2: /* song position */
        return 3;
    default: /* tune request and undefined */
        return 1;
    }
}

static int feed_status(mbox_midi_parser *p, uint8_t byte, uint8_t cable, uint8_t *out)
{
    if (byte == 0xf0) {
        p->running = 0;
        p->sysex = true;
        p->buf[0] = byte;
        p->len = 1;
        return 0;
    }
    p->running = byte < 0xf0 ? byte : 0;
    p->buf[0] = byte;
    p->len = 1;
    p->need = message_length(byte);
    return p->need == 1 ? emit(p, cable, out) : 0;
}

int mbox_midi_parser_feed(mbox_midi_parser *p, uint8_t byte, uint8_t cable,
                          uint8_t out[MBOX_MIDI_MAX_PACKETS_PER_BYTE * MBOX_MIDI_PACKET_SIZE])
{
    if (byte >= 0xf8) {
        /* Real-time bytes may interleave anything; send them on their own
         * without disturbing the message being assembled. */
        out[0] = byte;
        out[1] = 0;
        out[2] = 0;
        out[3] = (uint8_t)((cable & 0x0f) << 4 | 1);
        return 1;
    }

    if (p->sysex) {
        if (!(byte & 0x80) || byte == 0xf7) {
            p->buf[p->len++] = byte;
            if (byte == 0xf7)
                p->sysex = false;
            if (byte == 0xf7 || p->len == 3)
                return emit(p, cable, out);
            return 0;
        }
        /* Sysex cut short by a new status byte: flush the partial chunk,
         * then handle the status normally. */
        p->sysex = false;
        int n = p->len ? emit(p, cable, out) : 0;
        return n + feed_status(p, byte, cable, out + n * MBOX_MIDI_PACKET_SIZE);
    }

    if (byte & 0x80)
        return feed_status(p, byte, cable, out);

    /* Data byte. */
    if (p->len == 0) {
        if (!p->running)
            return 0; /* stray data byte */
        p->buf[0] = p->running;
        p->len = 1;
        p->need = message_length(p->running);
    }
    p->buf[p->len++] = byte;
    if (p->len == p->need)
        return emit(p, cable, out);
    return 0;
}

size_t mbox_midi_encode(mbox_midi_parser *p, const uint8_t *in, size_t n, uint8_t cable,
                        uint8_t *out, size_t out_cap, size_t *consumed)
{
    size_t written = 0;
    size_t i = 0;
    uint8_t tmp[MBOX_MIDI_MAX_PACKETS_PER_BYTE * MBOX_MIDI_PACKET_SIZE];
    while (i < n && written + sizeof tmp <= out_cap) {
        size_t len = (size_t)mbox_midi_parser_feed(p, in[i++], cable, tmp) * MBOX_MIDI_PACKET_SIZE;
        memcpy(out + written, tmp, len);
        written += len;
    }
    if (consumed)
        *consumed = i;
    return written;
}

size_t mbox_midi_decode(const uint8_t *packets, size_t n, uint8_t *out, size_t out_cap)
{
    size_t written = 0;
    for (size_t i = 0; i + MBOX_MIDI_PACKET_SIZE <= n; i += MBOX_MIDI_PACKET_SIZE) {
        uint8_t tag = packets[i + 3];
        if (tag == 0)
            continue;
        size_t len = tag & 0x03;
        if (written + len > out_cap)
            break;
        memcpy(out + written, packets + i, len);
        written += len;
    }
    return written;
}
