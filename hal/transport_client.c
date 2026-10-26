#include "transport_client.h"

#include <stdatomic.h>
#include <xpc/xpc.h>

#include "mbox/shm.h"

static xpc_connection_t connection;
static atomic_bool lost = true;

void *mbox_client_map(size_t *size)
{
    if (connection) {
        xpc_connection_cancel(connection);
        xpc_release(connection);
    }
    connection = xpc_connection_create_mach_service(MBOX_MACH_SERVICE, NULL, 0);
    if (!connection)
        return NULL;
    xpc_connection_set_event_handler(connection, ^(xpc_object_t event) {
        if (xpc_get_type(event) == XPC_TYPE_ERROR)
            atomic_store(&lost, true);
    });
    xpc_connection_resume(connection);

    xpc_object_t msg = xpc_dictionary_create(NULL, NULL, 0);
    xpc_dictionary_set_string(msg, "op", "map");
    xpc_object_t reply = xpc_connection_send_message_with_reply_sync(connection, msg);
    xpc_release(msg);

    void *base = NULL;
    if (xpc_get_type(reply) == XPC_TYPE_DICTIONARY &&
        xpc_dictionary_get_uint64(reply, "version") == MBOX_SHM_VERSION) {
        xpc_object_t shmem = xpc_dictionary_get_value(reply, "shmem");
        if (shmem) {
            void *addr = NULL;
            size_t n = xpc_shmem_map(shmem, &addr);
            if (n && mbox_shm_validate(addr, n)) {
                base = addr;
                *size = n;
                atomic_store(&lost, false);
            }
        }
    }
    xpc_release(reply);
    return base;
}

bool mbox_client_lost(void)
{
    return atomic_load(&lost);
}
