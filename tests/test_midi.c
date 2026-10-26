#include <string.h>

#include "check.h"
#include "mbox/midi.h"

static size_t roundtrip(const uint8_t *in, size_t n, uint8_t *out, size_t *packets)
{
    mbox_midi_parser p;
    mbox_midi_parser_reset(&p);
    uint8_t pk[256];
    size_t consumed;
    size_t w = mbox_midi_encode(&p, in, n, 0, pk, sizeof pk, &consumed);
    CHECK(consumed == n);
    *packets = w / 4;
    return mbox_midi_decode(pk, w, out, 256);
}

int main(void)
{
    uint8_t out[256];
    size_t packets;

    /* Note on: one 3-byte packet tagged with length 3, cable 0. */
    {
        const uint8_t in[] = {0x90, 0x3c, 0x64};
        mbox_midi_parser p;
        mbox_midi_parser_reset(&p);
        uint8_t pk[8];
        size_t c;
        CHECK(mbox_midi_encode(&p, in, 3, 2, pk, sizeof pk, &c) == 4);
        CHECK(pk[0] == 0x90 && pk[1] == 0x3c && pk[2] == 0x64 && pk[3] == 0x23);
    }

    /* Running status expands to full messages. */
    {
        const uint8_t in[] = {0x90, 0x3c, 0x64, 0x3e, 0x64, 0xc0, 0x05};
        size_t n = roundtrip(in, sizeof in, out, &packets);
        const uint8_t want[] = {0x90, 0x3c, 0x64, 0x90, 0x3e, 0x64, 0xc0, 0x05};
        CHECK(packets == 3);
        CHECK(n == sizeof want && memcmp(out, want, n) == 0);
    }

    /* Sysex is chunked by three and terminated with a short packet. */
    {
        const uint8_t in[] = {0xf0, 0x7e, 0x7f, 0x06, 0x01, 0xf7};
        size_t n = roundtrip(in, sizeof in, out, &packets);
        CHECK(packets == 2);
        CHECK(n == sizeof in && memcmp(out, in, n) == 0);
    }

    /* Real-time bytes interleave without breaking a message. */
    {
        const uint8_t in[] = {0x90, 0xf8, 0x3c, 0x64};
        size_t n = roundtrip(in, sizeof in, out, &packets);
        const uint8_t want[] = {0xf8, 0x90, 0x3c, 0x64};
        CHECK(packets == 2);
        CHECK(n == sizeof want && memcmp(out, want, n) == 0);
    }

    /* A sysex cut short by a one-byte status flushes both. */
    {
        const uint8_t in[] = {0xf0, 0x01, 0xf6};
        size_t n = roundtrip(in, sizeof in, out, &packets);
        CHECK(packets == 2);
        CHECK(n == 3 && out[0] == 0xf0 && out[1] == 0x01 && out[2] == 0xf6);
    }

    /* Stray data bytes are dropped; padding packets are skipped. */
    {
        const uint8_t in[] = {0x12, 0x34};
        CHECK(roundtrip(in, sizeof in, out, &packets) == 0 && packets == 0);
        const uint8_t pk[] = {0, 0, 0, 0, 0xfe, 0, 0, 0x01};
        CHECK(mbox_midi_decode(pk, sizeof pk, out, sizeof out) == 1 && out[0] == 0xfe);
    }
    return CHECK_DONE();
}
