/*
Group Details:
Member 1: Sumedh Jamadagni, 24CSB0A76
Member 2: Tarun Nagarajan 24CSB0A79
*/
#ifndef SIMPLEMAIL_COMMON_H
#define SIMPLEMAIL_COMMON_H

#define _POSIX_C_SOURCE 200809L
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define SM_USER_MAX 20
#define SM_SENDER_MAX 128
#define SM_PASS_MAX 30
#define SM_SUBJECT_MAX 512
#define SM_BODY_MAX 65536
#define SM_INITIAL_WINDOW 2048
#define SM_WINDOW_MAX 65536
#define SM_RECIPIENTS_MAX 16
#define SM_USER_COUNT 128
#define SM_QUEUE_CAPACITY 8
#define SM_SHM_NAME "/simplemail_v1_ring"
#define SM_ROOT "mailboxes"

typedef struct { char name[SM_USER_MAX + 1]; char pass[SM_PASS_MAX + 1]; } SmUser;
typedef struct { SmUser users[SM_USER_COUNT]; size_t count; } SmRegistry;

int sm_registry_load(const char *path, SmRegistry *registry, char *error, size_t error_size);
int sm_user_valid(const char *name);
int sm_password_valid(const char *password);
int sm_user_find(const SmRegistry *registry, const char *name);
void sm_canonicalize(char *name);
void sm_log(const char *component, const char *format, ...);
int sm_write_all(int fd, const void *data, size_t length);
int sm_read_line(int fd, char *buffer, size_t capacity, int timeout_seconds);
unsigned long sm_djb2(const char *text);
int sm_constant_time_equal(const char *a, const char *b);
int sm_random_nonce(char nonce[9]);
void sm_dot_stuff(const char *input, size_t length, char **output, size_t *output_length);

#endif
