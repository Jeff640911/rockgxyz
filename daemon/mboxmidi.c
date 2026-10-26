/*
 * mboxmidi: per-user LaunchAgent that exposes the Mbox's MIDI port as
 * CoreMIDI endpoints. mboxd runs as root outside any user session, where
 * CoreMIDI endpoints would be invisible to apps, so MIDI crosses over to
 * this agent through the FIFOs in the shared region.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <CoreMIDI/CoreMIDI.h>
#include <dispatch/dispatch.h>
#include <stdatomic.h>
#include <stdio.h>

#include "mbox/shm.h"
#include "../hal/transport_client.h"

#pragma clang diagnostic ignored "-Wdeprecated-declarations"

static _Atomic(mbox_shm_header *) g_region;
static MIDIEndpointRef g_source;

static void from_apps(const MIDIPacketList *list, void *ref, void *src_ref)
{
    mbox_shm_header *h = atomic_load(&g_region);
    if (!h)
        return;
    const MIDIPacket *p = &list->packet[0];
    for (UInt32 i = 0; i < list->numPackets; i++) {
        if (mbox_byte_ring_write(&h->midi_out, p->data, p->length) < p->length)
            fprintf(stderr, "mboxmidi: output FIFO full, dropping\n");
        p = MIDIPacketNext(p);
    }
}

static void to_apps(void *ctx)
{
    mbox_shm_header *h = atomic_load(&g_region);
    if (!h)
        return;
    uint8_t bytes[256];
    size_t n = mbox_byte_ring_read(&h->midi_in, bytes, sizeof bytes);
    if (!n)
        return;
    Byte storage[512];
    MIDIPacketList *list = (MIDIPacketList *)storage;
    MIDIPacket *p = MIDIPacketListInit(list);
    p = MIDIPacketListAdd(list, sizeof storage, p, 0, (ByteCount)n, bytes);
    if (p)
        MIDIReceived(g_source, list);
}

static void reconnect(void *ctx)
{
    if (atomic_load(&g_region) && !mbox_client_lost())
        return;
    size_t size;
    mbox_shm_header *h = mbox_client_map(&size);
    if (h)
        atomic_store(&g_region, h);
}

int main(void)
{
    MIDIClientRef client;
    MIDIEndpointRef dest;
    if (MIDIClientCreate(CFSTR("Mbox 2"), NULL, NULL, &client) != noErr ||
        MIDISourceCreate(client, CFSTR("Mbox 2 MIDI In"), &g_source) != noErr ||
        MIDIDestinationCreate(client, CFSTR("Mbox 2 MIDI Out"), from_apps, NULL, &dest) != noErr) {
        fprintf(stderr, "mboxmidi: cannot create CoreMIDI endpoints\n");
        return 1;
    }

    dispatch_queue_t q = dispatch_queue_create("com.rockgxyz.mboxmidi", DISPATCH_QUEUE_SERIAL);
    dispatch_source_t poll = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, q);
    dispatch_source_set_timer(poll, DISPATCH_TIME_NOW, NSEC_PER_MSEC, NSEC_PER_MSEC / 4);
    dispatch_source_set_event_handler_f(poll, to_apps);
    dispatch_resume(poll);

    dispatch_source_t retry = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, q);
    dispatch_source_set_timer(retry, DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC, NSEC_PER_SEC / 4);
    dispatch_source_set_event_handler_f(retry, reconnect);
    dispatch_resume(retry);

    CFRunLoopRun();
    return 0;
}
