#include "transport.h"

#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

#include "mbox/shm.h"

static void *region;
static size_t region_size;

#ifdef __APPLE__
#include <dispatch/dispatch.h>
#include <mach/mach.h>
#include <unistd.h>
#include <xpc/xpc.h>

static xpc_connection_t listener;
static xpc_object_t shmem;

static void handle_peer(xpc_connection_t peer)
{
    xpc_connection_set_event_handler(peer, ^(xpc_object_t msg) {
        if (xpc_get_type(msg) != XPC_TYPE_DICTIONARY)
            return; /* connection errors: nothing to clean up */
        xpc_object_t reply = xpc_dictionary_create_reply(msg);
        if (!reply)
            return;
        xpc_dictionary_set_uint64(reply, "version", MBOX_SHM_VERSION);
        xpc_dictionary_set_value(reply, "shmem", shmem);
        xpc_connection_send_message(peer, reply);
        xpc_release(reply);
    });
    xpc_connection_resume(peer);
}

void *mboxd_transport_start(size_t size)
{
    /* xpc_shmem_create needs page-aligned memory from vm_allocate. */
    vm_address_t addr = 0;
    size_t page = (size_t)getpagesize();
    size = (size + page - 1) & ~(page - 1);
    if (vm_allocate(mach_task_self(), &addr, size, VM_FLAGS_ANYWHERE) != KERN_SUCCESS)
        return NULL;
    region = (void *)addr;
    region_size = size;
    shmem = xpc_shmem_create(region, region_size);
    if (!shmem)
        return NULL;

    listener = xpc_connection_create_mach_service(MBOX_MACH_SERVICE, dispatch_get_global_queue(QOS_CLASS_USER_INTERACTIVE, 0),
                                                  XPC_CONNECTION_MACH_SERVICE_LISTENER);
    xpc_connection_set_event_handler(listener, ^(xpc_object_t peer) {
        if (xpc_get_type(peer) == XPC_TYPE_CONNECTION)
            handle_peer(peer);
    });
    xpc_connection_resume(listener);
    return region;
}

void mboxd_transport_stop(void)
{
    if (listener) {
        xpc_connection_cancel(listener);
        xpc_release(listener);
        listener = NULL;
    }
    if (shmem) {
        xpc_release(shmem);
        shmem = NULL;
    }
    if (region)
        vm_deallocate(mach_task_self(), (vm_address_t)region, region_size);
    region = NULL;
}

#else
#include <fcntl.h>
#include <unistd.h>

void *mboxd_transport_start(size_t size)
{
    int fd = shm_open(MBOX_SHM_POSIX_NAME, O_CREAT | O_RDWR, 0644);
    if (fd < 0) {
        perror("shm_open");
        return NULL;
    }
    if (ftruncate(fd, (off_t)size) != 0) {
        perror("ftruncate");
        close(fd);
        return NULL;
    }
    void *p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (p == MAP_FAILED) {
        perror("mmap");
        return NULL;
    }
    region = p;
    region_size = size;
    return region;
}

void mboxd_transport_stop(void)
{
    if (region)
        munmap(region, region_size);
    shm_unlink(MBOX_SHM_POSIX_NAME);
    region = NULL;
}
#endif
