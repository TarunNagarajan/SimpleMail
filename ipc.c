/*
Group Details:
Member 1: Sumedh Jamadagni, 24CSB0A76
Member 2: Tarun Nagarajan 24CSB0A79
*/
#define _POSIX_C_SOURCE 200809L
#include "ipc.h"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static int initialize(SmIpc *ipc, char *error, size_t error_size) {
    pthread_mutexattr_t ma;
    pthread_condattr_t ca;
    if (ftruncate(ipc->fd, (off_t)sizeof(SmSharedQueue)) < 0) goto fail;
    ipc->queue = mmap(NULL, sizeof(SmSharedQueue), PROT_READ | PROT_WRITE, MAP_SHARED, ipc->fd, 0);
    if (ipc->queue == MAP_FAILED) { ipc->queue = NULL; goto fail; }
    memset(ipc->queue, 0, sizeof(*ipc->queue));
    if (pthread_mutexattr_init(&ma) || pthread_condattr_init(&ca)) goto fail;
    if (pthread_mutexattr_setpshared(&ma, PTHREAD_PROCESS_SHARED) ||
        pthread_condattr_setpshared(&ca, PTHREAD_PROCESS_SHARED) ||
        pthread_mutex_init(&ipc->queue->mutex, &ma) ||
        pthread_cond_init(&ipc->queue->not_empty, &ca) ||
        pthread_cond_init(&ipc->queue->not_full, &ca)) {
        pthread_mutexattr_destroy(&ma); pthread_condattr_destroy(&ca); goto fail;
    }
    pthread_mutexattr_destroy(&ma); pthread_condattr_destroy(&ca);
    ipc->queue->next_ticket = 1;
    ipc->queue->owner_pid = getpid();
    __sync_synchronize(); ipc->queue->magic = SM_IPC_MAGIC;
    return 0;
fail:
    snprintf(error, error_size, "cannot initialize shared queue: %s", strerror(errno ? errno : EINVAL));
    return -1;
}

int sm_ipc_start(SmIpc *ipc, char *error, size_t error_size) {
    unsigned attempt;
    memset(ipc, 0, sizeof(*ipc)); ipc->fd = -1;
    for (attempt = 0; attempt < 2; ++attempt) {
        ipc->fd = shm_open(SM_SHM_NAME, O_RDWR | O_CREAT | O_EXCL, 0600);
        if (ipc->fd >= 0) {
            ipc->creator = 1;
            if (initialize(ipc, error, error_size) == 0) return 0;
            shm_unlink(SM_SHM_NAME); close(ipc->fd); ipc->fd = -1; return -1;
        }
        if (errno != EEXIST) break;
        ipc->fd = shm_open(SM_SHM_NAME, O_RDWR, 0600);
        if (ipc->fd < 0) continue;
        {
            struct stat st;
            if (fstat(ipc->fd, &st) < 0 || st.st_size < (off_t)sizeof(SmSharedQueue)) {
                close(ipc->fd); ipc->fd = -1; shm_unlink(SM_SHM_NAME); continue;
            }
        }
        {
            SmSharedQueue *header = mmap(NULL, sizeof(SmSharedQueue), PROT_READ, MAP_SHARED, ipc->fd, 0);
            if (header == MAP_FAILED) { close(ipc->fd); ipc->fd = -1; break; }
            uint32_t magic = header->magic;
            pid_t pid = header->owner_pid;
            munmap(header, sizeof(SmSharedQueue));
            /* Only the spooler creates the ring. A live owner means this is not a stale segment to replace. */
            if (magic == SM_IPC_MAGIC && pid > 0 && (kill(pid, 0) == 0 || errno == EPERM)) {
                snprintf(error, error_size, "another spooler owns the shared queue (pid %ld)", (long)pid);
                close(ipc->fd); ipc->fd = -1; return -1;
            }
        }
        close(ipc->fd); ipc->fd = -1; shm_unlink(SM_SHM_NAME);
    }
    if (ipc->fd < 0) snprintf(error, error_size, "cannot create shared queue: %s", strerror(errno));
    return -1;
}

int sm_ipc_join(SmIpc *ipc, char *error, size_t error_size) {
    unsigned i;
    memset(ipc, 0, sizeof(*ipc)); ipc->fd = -1;
    for (i = 0; i < 50; ++i) {
        ipc->fd = shm_open(SM_SHM_NAME, O_RDWR, 0600);
        if (ipc->fd >= 0) break;
        if (errno != ENOENT) break;
        struct timespec pause = {0, 100000000}; nanosleep(&pause, NULL);
    }
    if (ipc->fd < 0) { snprintf(error, error_size, "cannot open spooler shared queue: %s", strerror(errno)); return -1; }
    ipc->queue = mmap(NULL, sizeof(SmSharedQueue), PROT_READ | PROT_WRITE, MAP_SHARED, ipc->fd, 0);
    if (ipc->queue == MAP_FAILED) { ipc->queue = NULL; snprintf(error, error_size, "cannot map shared queue: %s", strerror(errno)); close(ipc->fd); return -1; }
    if (ipc->queue->magic != SM_IPC_MAGIC || ipc->queue->owner_pid <= 0) {
        snprintf(error, error_size, "shared queue is not initialized"); munmap(ipc->queue, sizeof(SmSharedQueue)); close(ipc->fd); ipc->queue = NULL; return -1;
    }
    return 0;
}

void sm_ipc_close(SmIpc *ipc) {
    if (ipc->queue) munmap(ipc->queue, sizeof(SmSharedQueue));
    if (ipc->fd >= 0) close(ipc->fd);
    if (ipc->creator) shm_unlink(SM_SHM_NAME);
    ipc->queue = NULL; ipc->fd = -1;
}

int sm_ipc_push(SmSharedQueue *q, SmMessage *message, uint64_t *ticket) {
    pthread_mutex_lock(&q->mutex);
    while (q->count == SM_QUEUE_CAPACITY) pthread_cond_wait(&q->not_full, &q->mutex);
    message->ticket = q->next_ticket++;
    q->entries[q->tail] = *message;
    q->tail = (q->tail + 1) % SM_QUEUE_CAPACITY; ++q->count;
    if (ticket) *ticket = message->ticket;
    pthread_cond_signal(&q->not_empty); pthread_mutex_unlock(&q->mutex); return 0;
}

int sm_ipc_try_push(SmSharedQueue *q, SmMessage *message, uint64_t *ticket) {
    /* One mutex protects the count and both indices, so readers never see half-published records. */
    pthread_mutex_lock(&q->mutex);
    if (q->count == SM_QUEUE_CAPACITY) { pthread_mutex_unlock(&q->mutex); return 1; }
    message->ticket = q->next_ticket++;
    q->entries[q->tail] = *message;
    q->tail = (q->tail + 1) % SM_QUEUE_CAPACITY; ++q->count;
    if (ticket) *ticket = message->ticket;
    pthread_cond_signal(&q->not_empty); pthread_mutex_unlock(&q->mutex); return 0;
}

int sm_ipc_pop(SmSharedQueue *q, SmMessage *message) {
    pthread_mutex_lock(&q->mutex);
    while (!q->count) {
        struct timespec deadline;
        clock_gettime(CLOCK_REALTIME, &deadline); ++deadline.tv_sec;
        if (pthread_cond_timedwait(&q->not_empty, &q->mutex, &deadline) == ETIMEDOUT) {
            pthread_mutex_unlock(&q->mutex); return 1;
        }
    }
    *message = q->entries[q->head];
    memset(&q->entries[q->head], 0, sizeof(SmMessage));
    q->head = (q->head + 1) % SM_QUEUE_CAPACITY; --q->count; ++q->freed_generation;
    pthread_cond_signal(&q->not_full); pthread_mutex_unlock(&q->mutex); return 0;
}

size_t sm_ipc_count(SmSharedQueue *q) {
    size_t n; pthread_mutex_lock(&q->mutex); n = q->count; pthread_mutex_unlock(&q->mutex); return n;
}
