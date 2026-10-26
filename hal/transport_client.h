/* Plug-in side of daemon/transport.c: maps mboxd's shared memory. */
#ifndef MBOX_HAL_TRANSPORT_CLIENT_H
#define MBOX_HAL_TRANSPORT_CLIENT_H

#include <stdbool.h>
#include <stddef.h>

/* Connects to mboxd and maps its region. Blocks for up to a second; never
 * call it from the IO thread. Returns NULL if the daemon is not running. */
void *mbox_client_map(size_t *size);
/* True once the connection to the daemon that provided the current
 * mapping has been lost (daemon exited or restarted). */
bool mbox_client_lost(void);

#endif
