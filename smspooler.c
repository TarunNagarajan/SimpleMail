/* Group Details: Member 1 Name: Sumedh Jamadagni, Member 1 Roll number: 24CSB0A76, Member 2 Name: Tarun Nagarajan, Member 2 Roll number: 24CSB0A79 */
#define _POSIX_C_SOURCE 200809L
#include "common.h"
#include "ipc.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t stopping;
static SmIpc ipc;

static void on_signal(int sig) { (void)sig; stopping = 1; }

static int ensure_directory(const char *path) {
    if (mkdir(path, 0700) == 0 || errno == EEXIST) {
        struct stat st;
        return stat(path, &st) == 0 && S_ISDIR(st.st_mode) ? 0 : -1;
    }
    return -1;
}

static int ensure_mailbox_directory(const char *path, int *created) {
    struct stat st;
    if (mkdir(path, 0700) == 0) *created = 1;
    else if (errno == EEXIST) *created = 0;
    else return -1;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode) ? 0 : -1;
}

static int mailbox_has_files(const char *dir) {
    DIR *d = opendir(dir); struct dirent *entry;
    if (!d) return -1;
    while ((entry = readdir(d)) != NULL) {
        if (entry->d_name[0] != '.') { closedir(d); errno = EINVAL; return 1; }
    }
    closedir(d); return 0;
}

static int read_next_id(const char *dir, unsigned long long *next, int allow_new_mailbox) {
    char path[640], raw[64]; FILE *f; size_t length;
    snprintf(path, sizeof(path), "%s/.next-id", dir);
    f = fopen(path, "r");
    if (!f) {
        if (errno != ENOENT) return -1;
        /* Only a directory created in this startup has no prior deletion history to lose. */
        if (!allow_new_mailbox || mailbox_has_files(dir) != 0) { errno = EINVAL; return -1; }
        *next = 1; return 0;
    }
    length = fread(raw, 1, sizeof(raw), f);
    int invalid = ferror(f) || length == sizeof(raw);
    if (fclose(f) != 0) invalid = 1;
    if (invalid) { errno = EINVAL; return -1; }
    if (length && raw[length - 1] == '\n') --length;
    if (length && raw[length - 1] == '\r') --length;
    if (!length) { errno = EINVAL; return -1; }
    for (size_t i = 0; i < length; ++i)
        if (raw[i] < '0' || raw[i] > '9') { errno = EINVAL; return -1; }
    raw[length] = '\0'; errno = 0; char *end;
    *next = strtoull(raw, &end, 10);
    if (errno || *end || !*next) { errno = EINVAL; return -1; }
    return 0;
}

static int store_next_id(const char *dir, unsigned long long next) {
    char path[640], temp[640]; FILE *f;
    snprintf(path, sizeof(path), "%s/.next-id", dir);
    snprintf(temp, sizeof(temp), "%s/.next-id.tmp", dir);
    f = fopen(temp, "w"); if (!f) return -1;
    /* Reserve the next number durably before creating its mail file; crashes can skip IDs but never reuse them. */
    if (fprintf(f, "%llu\n", next) < 0 || fflush(f) < 0 || fsync(fileno(f)) < 0) { fclose(f); unlink(temp); return -1; }
    if (fclose(f) < 0 || rename(temp, path) < 0) { unlink(temp); return -1; }
    int dirfd = open(dir, O_RDONLY);
    if (dirfd < 0) return -1;
    int synced = fsync(dirfd); close(dirfd);
    return synced;
}

static int persist(const SmMessage *m, const char *recipient) {
    char dir[512], path[640], temp[660], recipients[SM_RECIPIENTS_MAX * (SM_USER_MAX + 1)], date[32];
    unsigned long long id;
    FILE *file;
    time_t now = time(NULL); struct tm local;
    size_t used = 0;
    snprintf(dir, sizeof(dir), "%s/%s", SM_ROOT, recipient);
    if (ensure_directory(SM_ROOT) || ensure_directory(dir)) return -1;
    if (read_next_id(dir, &id, 0)) return -1;
    if (id == ULLONG_MAX || store_next_id(dir, id + 1)) return -1;
    snprintf(path, sizeof(path), "%s/%llu.txt", dir, id);
    snprintf(temp, sizeof(temp), "%s/.%llu.tmp", dir, id);
    recipients[0] = '\0';
    for (size_t i = 0; i < m->recipient_count; ++i) {
        int n = snprintf(recipients + used, sizeof(recipients) - used, "%s%s", i ? "," : "", m->recipients[i]);
        if (n < 0 || (size_t)n >= sizeof(recipients) - used) return -1;
        used += (size_t)n;
    }

    localtime_r(&now, &local); strftime(date, sizeof(date), "%Y-%m-%d %H:%M:%S", &local);
    file = fopen(temp, "wb"); if (!file) return -1;

    if (fprintf(file, "From: %s\nTo: %s\nSubject: %s\nDate: %s\n---\n\n",
                m->sender, recipients, m->subject, date) < 0 ||
        fwrite(m->body, 1, m->body_length, file) != m->body_length || fflush(file) < 0 || fsync(fileno(file)) < 0) {
        fclose(file); unlink(temp); return -1;
    }

    if (fclose(file) < 0 || rename(temp, path) < 0) { unlink(temp); return -1; }
    return 0;
}

int main(int argc, char **argv) {
    SmRegistry registry; SmMessage message; char error[256];
    if (argc != 2) { fprintf(stderr, "usage: %s <userfile>\n", argv[0]); return 2; }
    if (sm_registry_load(argv[1], &registry, error, sizeof(error))) { fprintf(stderr, "smspooler: %s\n", error); return 1; }
    if (ensure_directory(SM_ROOT)) { fprintf(stderr, "smspooler: cannot create %s: %s\n", SM_ROOT, strerror(errno)); return 1; }

    for (size_t i = 0; i < registry.count; ++i) {
        char dir[512]; unsigned long long next; int created;
        snprintf(dir, sizeof(dir), "%s/%s", SM_ROOT, registry.users[i].name);
        if (ensure_mailbox_directory(dir, &created)) { fprintf(stderr, "smspooler: cannot create mailbox %s: %s\n", registry.users[i].name, strerror(errno)); return 1; }
        if (read_next_id(dir, &next, created) || next == ULLONG_MAX || store_next_id(dir, next)) {
            fprintf(stderr, "smspooler: unsafe or unavailable ID metadata for mailbox %s: %s\n", registry.users[i].name, strerror(errno)); return 1;
        }
    }

    if (sm_ipc_start(&ipc, error, sizeof(error))) { fprintf(stderr, "smspooler: %s\n", error); return 1; }
    signal(SIGINT, on_signal); signal(SIGTERM, on_signal); signal(SIGPIPE, SIG_IGN);
    sm_log("spooler", "ready; %zu registered users, queue capacity %d", registry.count, SM_QUEUE_CAPACITY);

    while (!stopping) {
        if (sm_ipc_pop(ipc.queue, &message) != 0) continue;
        if (stopping) break;
        sm_log("spooler", "ticket %llu dequeued (%zu recipients)", (unsigned long long)message.ticket, message.recipient_count);
        for (size_t i = 0; i < message.recipient_count; ++i) {
            if (sm_user_find(&registry, message.recipients[i]) < 0) {
                sm_log("spooler", "ticket %llu has unknown recipient '%s'", (unsigned long long)message.ticket, message.recipients[i]); continue;
            }
            if (persist(&message, message.recipients[i]))
                sm_log("spooler", "delivery failed: ticket %llu to %s: %s", (unsigned long long)message.ticket, message.recipients[i], strerror(errno));
            else sm_log("spooler", "ticket %llu delivered to %s", (unsigned long long)message.ticket, message.recipients[i]);
        }
    }

    sm_log("spooler", "stopping");
    sm_ipc_close(&ipc);
    return 0;
}
