/*
 * MIDI framing used by the Mbox 2.
 *
 * The device speaks the "Midiman" variant of USB MIDI: 4-byte packets
 * where bytes 0..2 are raw MIDI bytes and byte 3 is (cable << 4) | length.
 * A packet whose byte 3 is zero is padding.
 */
#ifndef MBOX_MIDI_H
#define MBOX_MIDI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MBOX_MIDI_PACKET_SIZE 4
/* An aborted sysex flushes its partial chunk plus the new status byte. */
#define MBOX_MIDI_MAX_PACKETS_PER_BYTE 2

typedef struct {
    uint8_t buf[3];
    uint8_t len;
    uint8_t need;    /* total bytes of the message being assembled */
    uint8_t running; /* running status, 0 = none */
    bool sysex;
} mbox_midi_parser;

void mbox_midi_parser_reset(mbox_midi_parser *p);

/* Feeds one raw MIDI byte and returns how many packets (0..2) were
 * completed and written to `out`. */
int mbox_midi_parser_feed(mbox_midi_parser *p, uint8_t byte, uint8_t cable,
                          uint8_t out[MBOX_MIDI_MAX_PACKETS_PER_BYTE * MBOX_MIDI_PACKET_SIZE]);

/* Encodes as many bytes of `in` as fit into `out` (stopping while at least
 * 8 bytes of room remain); returns bytes written to `out` (a multiple of 4)
 * and sets *consumed to bytes taken from `in`. */
size_t mbox_midi_encode(mbox_midi_parser *p, const uint8_t *in, size_t n, uint8_t cable,
                        uint8_t *out, size_t out_cap, size_t *consumed);

/* Unpacks a buffer of packets into raw MIDI bytes; returns bytes written. */
size_t mbox_midi_decode(const uint8_t *packets, size_t n, uint8_t *out, size_t out_cap);

#ifdef __cplusplus
}
#endif

#endif
