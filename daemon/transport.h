/*
 * How mboxd hands its shared memory to clients.
 *
 * macOS: an XPC mach service (declared in the launchd plist and in the
 * plug-in's AudioServerPlugIn_MachServices) replies to every connection
 * with an xpc_shmem object for the region. coreaudiod's sandbox allows
 * exactly this, while it blocks POSIX shared memory.
 *
 * Elsewhere: a POSIX shared memory object, for development and tests.
 */
#ifndef MBOXD_TRANSPORT_H
#define MBOXD_TRANSPORT_H

#include <stddef.h>

/* Allocates `size` bytes of shareable, zeroed memory and starts serving
 * it. Returns NULL on failure. */
void *mboxd_transport_start(size_t size);
void mboxd_transport_stop(void);

#endif
