/*
Group Details:
Member 1: Sumedh Jamadagni, 24CSB0A76
Member 2: Tarun Nagarajan 24CSB0A79
*/
#ifndef SIMPLEMAIL_IPC_H
#define SIMPLEMAIL_IPC_H
#include "common.h"
#include <pthread.h>
#include <sys/types.h>

#define SM_IPC_MAGIC 0x534d5232u
typedef struct {
    uint64_t ticket;
    char sender[SM_SENDER_MAX + 1];
    char recipients[SM_RECIPIENTS_MAX][SM_USER_MAX + 1];
    size_t recipient_count;
    char subject[SM_SUBJECT_MAX + 1];
    unsigned char body[SM_BODY_MAX];
    size_t body_length;
} SmMessage;

typedef struct {
    uint32_t magic;
    pid_t owner_pid;
    pthread_mutex_t mutex;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
    size_t head, tail, count;
    uint64_t next_ticket;
    uint64_t freed_generation;
    SmMessage entries[SM_QUEUE_CAPACITY];
} SmSharedQueue;

typedef struct { int fd; SmSharedQueue *queue; int creator; } SmIpc;
int sm_ipc_start(SmIpc *ipc, char *error, size_t error_size);
int sm_ipc_join(SmIpc *ipc, char *error, size_t error_size);
void sm_ipc_close(SmIpc *ipc);
int sm_ipc_push(SmSharedQueue *queue, SmMessage *message, uint64_t *ticket);
int sm_ipc_try_push(SmSharedQueue *queue, SmMessage *message, uint64_t *ticket);
int sm_ipc_pop(SmSharedQueue *queue, SmMessage *message);
size_t sm_ipc_count(SmSharedQueue *queue);
#endif
