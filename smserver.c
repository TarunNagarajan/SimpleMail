/* Group Details: Member 1 Name: Sumedh Jamadagni, Member 1 Roll number: 24CSB0A76, Member 2 Name: Tarun Nagarajan, Member 2 Roll number: 24CSB0A79 */
#define _POSIX_C_SOURCE 200809L
#include "common.h"
#include "ipc.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <dirent.h>
#include <poll.h>
#include <stdarg.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define CLIENT_MAX 128
#define INPUT_CAP (SM_BODY_MAX * 2 + 2048)
#define OUTPUT_CAP (SM_BODY_MAX * 3 + 65536)
enum { ST_MODE, ST_AUTH, ST_MAIL, ST_FROM, ST_TO, ST_SUB, ST_BODY_KIND, ST_STREAM, ST_LEGACY, ST_WAIT_QUEUE };

typedef struct {
    int fd, state, attempts, close_after_output;
    time_t accepted;
    char input[INPUT_CAP]; size_t input_length;
    char *output; size_t output_length, output_offset;
    char nonce[9], username[SM_USER_MAX + 1];
    SmMessage message;
    size_t credits, chunk_pending;
} Client;
typedef struct { char from[SM_SENDER_MAX + 1], to[SM_RECIPIENTS_MAX * (SM_USER_MAX + 1)], subject[SM_SUBJECT_MAX + 1], date[32]; size_t bytes; unsigned char *body; } StoredMail;

static volatile sig_atomic_t stopping;
static Client clients[CLIENT_MAX];
static uint64_t last_freed_generation;
static void on_signal(int sig) { (void)sig; stopping = 1; }

static int queue_output(Client *c, const void *data, size_t length) {
    char *grown;
    if (length > OUTPUT_CAP - c->output_length) return -1;
    if (c->output_offset) {
        memmove(c->output, c->output + c->output_offset, c->output_length - c->output_offset);
        c->output_length -= c->output_offset; c->output_offset = 0;
    }
    grown = realloc(c->output, c->output_length + length + 1);
    if (!grown) return -1;
    c->output = grown; memcpy(c->output + c->output_length, data, length); c->output_length += length;
    return 0;
}

static int reply(Client *c, const char *format, ...) {
    char small[1024]; va_list ap; int n;
    va_start(ap, format); n = vsnprintf(small, sizeof(small), format, ap); va_end(ap);
    if (n < 0 || (size_t)n >= sizeof(small)) return -1;
    return queue_output(c, small, (size_t)n);
}

static void consume(Client *c, size_t count) {
    if (count >= c->input_length) c->input_length = 0;
    else { memmove(c->input, c->input + count, c->input_length - count); c->input_length -= count; }
}

static int printable_text(const char *text) {
    for (const unsigned char *p = (const unsigned char *)text; *p; ++p)
        if (*p < 32 || *p > 126) return 0;
    return 1;
}

static void drop_client(Client *c) {
    sm_log("server", "client disconnected");
    if (c->fd >= 0) close(c->fd);
    free(c->output); memset(c, 0, sizeof(*c)); c->fd = -1;
}

/* TCP reads don't preserve lines; leave incomplete frames buffered until CRLF arrives. */
static int take_line(Client *c, char *line, size_t cap) {
    size_t i;
    for (i = 0; i + 1 < c->input_length; ++i) {
        if (c->input[i] == '\r' && c->input[i + 1] == '\n') {
            size_t n = i + 2;
            if (n > 512) return -2;
            if (i + 1 >= cap) return -2;
            memcpy(line, c->input, i); line[i] = '\0'; consume(c, n); return (int)i + 1;
        }
        if (c->input[i] == '\n' && (i == 0 || c->input[i - 1] != '\r')) return -2;
        if (i >= 510) return -2;
    }
    if (c->input_length > 510) return -2;
    return 0;
}

static int parse_id(const char *text, unsigned long long *id) {
    char *end; errno = 0; *id = strtoull(text, &end, 10);
    return !errno && *text && !*end && *id > 0;
}

static int mailbox_path(char *path, size_t cap, const char *user) {
    if (!sm_user_valid(user)) return -1;
    if (snprintf(path, cap, "%s/%s", SM_ROOT, user) >= (int)cap) return -1;
    return 0;
}

static int load_mail(const char *dir, unsigned long long id, StoredMail *mail) {
    char path[640], line[1200]; FILE *f;
    snprintf(path, sizeof(path), "%s/%llu.txt", dir, id); f = fopen(path, "rb"); if (!f) return -1;
    memset(mail, 0, sizeof(*mail));
    if (!fgets(line, sizeof(line), f) || strncmp(line, "From: ", 6)) { fclose(f); return -1; }
    line[strcspn(line, "\r\n")] = '\0';
    if (strlen(line + 6) > SM_SENDER_MAX) { fclose(f); return -1; }
    strcpy(mail->from, line + 6);
    if (!fgets(line, sizeof(line), f) || strncmp(line, "To: ", 4)) { fclose(f); return -1; }
    line[strcspn(line, "\r\n")] = '\0';
    if (strlen(line + 4) >= sizeof(mail->to)) { fclose(f); return -1; }
    strcpy(mail->to, line + 4);
    if (!fgets(line, sizeof(line), f) || strncmp(line, "Subject: ", 9)) { fclose(f); return -1; }
    line[strcspn(line, "\r\n")] = '\0';
    if (strlen(line + 9) > SM_SUBJECT_MAX) { fclose(f); return -1; }
    strcpy(mail->subject, line + 9);
    if (!fgets(line, sizeof(line), f) || strncmp(line, "Date: ", 6)) { fclose(f); return -1; }
    line[strcspn(line, "\r\n")] = '\0';
    if (strlen(line + 6) >= sizeof(mail->date)) { fclose(f); return -1; }
    strcpy(mail->date, line + 6);
    if (!fgets(line, sizeof(line), f) || (strcmp(line, "---\n") && strcmp(line, "---\r\n")) ||
        !fgets(line, sizeof(line), f) || (strcmp(line, "\n") && strcmp(line, "\r\n"))) { fclose(f); return -1; }
    mail->body = malloc(SM_BODY_MAX + 1); if (!mail->body) { fclose(f); return -1; }
    mail->bytes = fread(mail->body, 1, SM_BODY_MAX + 1, f);
    if (mail->bytes > SM_BODY_MAX || ferror(f)) { free(mail->body); fclose(f); return -1; }
    fclose(f); return 0;
}

static int numeric_ids(const char *dir, unsigned long long **ids, size_t *count) {
    DIR *d = opendir(dir); struct dirent *entry; size_t cap = 8;
    *count = 0; *ids = malloc(cap * sizeof(**ids)); if (!*ids) { if (d) closedir(d); return -1; }
    if (!d) return 0;
    while ((entry = readdir(d)) != NULL) {
        unsigned long long id; char *end; errno = 0; id = strtoull(entry->d_name, &end, 10);
        if (errno || end == entry->d_name || strcmp(end, ".txt") || !id) continue;
        if (*count == cap) { cap *= 2; unsigned long long *p = realloc(*ids, cap * sizeof(**ids)); if (!p) { free(*ids); closedir(d); return -1; } *ids = p; }
        (*ids)[(*count)++] = id;
    }
    closedir(d); return 0;
}

static int compare_ids(const void *a, const void *b) {
    unsigned long long x = *(const unsigned long long *)a, y = *(const unsigned long long *)b;
    return x < y ? -1 : x > y;
}

static int handle_list(Client *c) {
    char dir[512]; unsigned long long *ids; size_t count;
    if (mailbox_path(dir, sizeof(dir), c->username) || numeric_ids(dir, &ids, &count)) return reply(c, "ERR Mailbox unavailable\r\n");
    qsort(ids, count, sizeof(*ids), compare_ids);
    if (reply(c, "OK %zu messages\r\n", count)) { free(ids); return -1; }
    for (size_t i = 0; i < count; ++i) {
        StoredMail mail;
        if (!load_mail(dir, ids[i], &mail)) {
            if (reply(c, "%llu\t%s\t%s\t%s\r\n", ids[i], mail.from, mail.subject, mail.date)) { free(mail.body); free(ids); return -1; }
            free(mail.body);
        }
    }
    free(ids); return reply(c, ".\r\n");
}

static int handle_read(Client *c, const char *arg) {
    char dir[512], *escaped; size_t escaped_n; unsigned long long id; StoredMail mail;
    if (!parse_id(arg, &id) || mailbox_path(dir, sizeof(dir), c->username) || load_mail(dir, id, &mail)) return reply(c, "ERR No such message\r\n");
    if (reply(c, "OK\r\nFrom: %s\r\nTo: %s\r\nSubject: %s\r\nDate: %s\r\n---\r\n\r\n",
              mail.from, mail.to, mail.subject, mail.date)) { free(mail.body); return -1; }
    sm_dot_stuff((char *)mail.body, mail.bytes, &escaped, &escaped_n);
    if (!escaped) { free(mail.body); return -1; }
    sm_log("server", "mailbox %s read message %llu", c->username, id);
    if (queue_output(c, escaped, escaped_n) || (escaped_n && escaped[escaped_n - 1] != '\n' && queue_output(c, "\r\n", 2)) || queue_output(c, ".\r\n", 3)) {
        free(escaped); free(mail.body); return -1;
    }
    free(escaped); free(mail.body); return 0;
}

static int handle_delete(Client *c, const char *arg) {
    char dir[512], path[600]; unsigned long long id;
    if (!parse_id(arg, &id) || mailbox_path(dir, sizeof(dir), c->username)) return reply(c, "ERR Invalid ID\r\n");
    snprintf(path, sizeof(path), "%s/%llu.txt", dir, id);
    if (unlink(path) < 0) return reply(c, "ERR No such message\r\n");
    sm_log("server", "mailbox %s deleted message %llu", c->username, id);
    return reply(c, "OK Deleted.\r\n");
}

static int handle_count(Client *c) {
    char dir[512]; unsigned long long *ids; size_t count;
    if (mailbox_path(dir, sizeof(dir), c->username) || numeric_ids(dir, &ids, &count)) return reply(c, "ERR Mailbox unavailable\r\n");
    free(ids); return reply(c, "OK %zu.\r\n", count);
}

static int finish_message(Client *c, SmSharedQueue *queue) {
    uint64_t ticket;
    if (!c->message.subject[0]) strcpy(c->message.subject, "(no subject)");
    if (sm_ipc_try_push(queue, &c->message, &ticket)) { c->state = ST_WAIT_QUEUE; return 0; }
    sm_log("server", "queued ticket %llu from %s to %zu mailboxes", (unsigned long long)ticket, c->message.sender, c->message.recipient_count);
    if (reply(c, "OK Queued %llu to %zu mailboxes\r\n", (unsigned long long)ticket, c->message.recipient_count)) return -1;
    memset(&c->message, 0, sizeof(c->message)); c->state = ST_FROM; c->credits = c->chunk_pending = 0; return 0;
}

static int command_line(Client *c, SmRegistry *registry, SmSharedQueue *queue, char *line) {
    char a[80], extra;
    if (c->state == ST_MODE) {
        if (!strcmp(line, "MODE SEND")) { c->state = ST_FROM; sm_log("server", "selected SEND mode"); return reply(c, "OK SEND\r\n"); }
        if (!strcmp(line, "MODE RECV")) {
            if (sm_random_nonce(c->nonce)) return -1;
            c->state = ST_AUTH; sm_log("server", "selected RECV mode"); return reply(c, "AUTH REQUIRED %s\r\n", c->nonce);
        }
        c->close_after_output = 1; return reply(c, "ERR Expected MODE SEND or MODE RECV\r\n");
    }
    if (c->state == ST_AUTH) {
        char canonical[SM_USER_MAX + 1], payload[SM_PASS_MAX + 9], expected[64]; unsigned long claimed;
        int index;
        if (sscanf(line, "AUTH %21s %lu %c", a, &claimed, &extra) != 2 || !sm_user_valid(a)) index = -1;
        else {
            strcpy(canonical, a); sm_canonicalize(canonical); index = sm_user_find(registry, canonical);
        }
        if (index >= 0) {
            snprintf(payload, sizeof(payload), "%s%s", registry->users[index].pass, c->nonce);
            snprintf(expected, sizeof(expected), "%lu", sm_djb2(payload));
            char provided[64]; snprintf(provided, sizeof(provided), "%lu", claimed);
            if (sm_constant_time_equal(expected, provided)) {
                strcpy(c->username, registry->users[index].name); c->state = ST_MAIL;
                sm_log("server", "authenticated mailbox %s", c->username); return reply(c, "OK Welcome %s\r\n", c->username);
            }
        }
        ++c->attempts;
        sm_log("server", "authentication failure (%d/3)", c->attempts);
        if (c->attempts >= 3) { c->close_after_output = 1; return reply(c, "ERR Too many failures\r\n"); }
        return reply(c, "ERR Authentication failed\r\n");
    }
    if (c->state == ST_MAIL) {
        if (!strcmp(line, "LIST")) { sm_log("server", "mailbox %s listed", c->username); return handle_list(c); }
        if (!strcmp(line, "COUNT")) { sm_log("server", "mailbox %s counted", c->username); return handle_count(c); }
        if (sscanf(line, "READ %79s %c", a, &extra) == 1) return handle_read(c, a);
        if (sscanf(line, "DELETE %79s %c", a, &extra) == 1) return handle_delete(c, a);
        if (!strcmp(line, "QUIT")) { sm_log("server", "mailbox %s logged out", c->username); c->close_after_output = 1; return reply(c, "BYE\r\n"); }
        return reply(c, "ERR Unknown command.\r\n");
    }
    if (c->state == ST_FROM) {
        if (!strcmp(line, "QUIT")) { c->close_after_output = 1; return reply(c, "BYE\r\n"); }
        if (strncmp(line, "FROM ", 5) || !line[5] || strlen(line + 5) > SM_SENDER_MAX || !printable_text(line + 5)) return reply(c, "ERR Expected FROM <display name>\r\n");
        memset(&c->message, 0, sizeof(c->message)); strcpy(c->message.sender, line + 5);
        c->state = ST_TO; return reply(c, "OK Sender accepted\r\n");
    }
    if (c->state == ST_TO) {
        if (sscanf(line, "TO %21s %c", a, &extra) == 1 && sm_user_valid(a)) {
            int index = sm_user_find(registry, a);
            if (index < 0) return reply(c, "ERR Unknown recipient\r\n");
            for (size_t i = 0; i < c->message.recipient_count; ++i) if (!strcmp(c->message.recipients[i], registry->users[index].name)) return reply(c, "ERR Duplicate recipient\r\n");
            if (c->message.recipient_count == SM_RECIPIENTS_MAX) return reply(c, "ERR Too many recipients\r\n");
            strcpy(c->message.recipients[c->message.recipient_count++], registry->users[index].name);
            return reply(c, "OK Recipient accepted\r\n");
        }
        if ((!strcmp(line, "SUB") || !strncmp(line, "SUB ", 4)) && c->message.recipient_count) {
            const char *subject = line[3] == ' ' ? line + 4 : "";
            if (strlen(subject) > SM_SUBJECT_MAX || !printable_text(subject)) return reply(c, "ERR Invalid subject\r\n");
            strcpy(c->message.subject, subject); c->state = ST_BODY_KIND; return reply(c, "OK Subject accepted\r\n");
        }
        return reply(c, "ERR Expected TO or SUB after at least one recipient\r\n");
    }
        if (c->state == ST_BODY_KIND) {
        if (!strcmp(line, "BODY_STREAM")) { c->state = ST_STREAM; c->credits = SM_INITIAL_WINDOW; return reply(c, "OK Window %zu\r\n", c->credits); }
        if (!strcmp(line, "BODY")) { c->state = ST_LEGACY; return reply(c, "OK Send body; terminate with CRLF.CRLF\r\n"); }
        return reply(c, "ERR Expected BODY_STREAM or BODY\r\n");
    }
    if (c->state == ST_STREAM) {
        if (!strcmp(line, "END_BODY")) return finish_message(c, queue);
        if (sscanf(line, "CHUNK %79s %c", a, &extra) != 1 || !*a) return reply(c, "ERR Expected CHUNK <byte_count> or END_BODY\r\n");
        char *end; errno = 0; unsigned long long n = strtoull(a, &end, 10);
        if (errno || *end || n > SM_BODY_MAX || n > c->credits || c->message.body_length + n > SM_BODY_MAX) {
            c->close_after_output = 1; return reply(c, n > c->credits ? "ERR Window overflow\r\n" : "ERR Body too large\r\n");
        }
        c->chunk_pending = (size_t)n; c->state = ST_STREAM + 10; return 0;
    }
    return reply(c, "ERR Invalid protocol state\r\n");
}

static int process_input(Client *c, SmRegistry *registry, SmSharedQueue *queue) {
    int progress = 1;
    while (progress && !c->close_after_output) {
        progress = 0;
        if (c->state == ST_WAIT_QUEUE) break;
        if (c->state == ST_STREAM + 10) {
            if (c->input_length < c->chunk_pending) break;
            memcpy(c->message.body + c->message.body_length, c->input, c->chunk_pending);
            c->message.body_length += c->chunk_pending; c->credits -= c->chunk_pending; consume(c, c->chunk_pending);
            /* Moving received bytes into the bounded body frees this input-window space immediately. */
            c->credits += c->chunk_pending;
            c->state = ST_STREAM;
            if (reply(c, "OK Window %zu\r\n", c->credits)) return -1;
            progress = 1; continue;
        }
        if (c->state == ST_LEGACY) {
            size_t i;
            for (i = 0; i + 5 <= c->input_length; ++i) if (!memcmp(c->input + i, "\r\n.\r\n", 5)) break;
            if (i + 5 > c->input_length) {
                if (c->message.body_length + c->input_length > SM_BODY_MAX + 3) { c->close_after_output = 1; return reply(c, "ERR Body too large\r\n"); }
                break;
            }
            size_t raw = i + 2;
            if (c->message.body_length + raw > SM_BODY_MAX) { c->close_after_output = 1; return reply(c, "ERR Body too large\r\n"); }
            /* SMTP dot unstuffing removes one leading dot on each line. */
            int line_start = c->message.body_length == 0 || (c->message.body_length && c->message.body[c->message.body_length - 1] == '\n');
            for (size_t j = 0; j < raw; ++j) {
                if (line_start && c->input[j] == '.' && j + 1 < raw && c->input[j + 1] == '.') ++j;
                c->message.body[c->message.body_length++] = (unsigned char)c->input[j];
                line_start = c->input[j] == '\n';
            }
            consume(c, i + 5); if (finish_message(c, queue)) return -1; progress = 1; continue;
        }
        char line[512]; int n = take_line(c, line, sizeof(line));
        if (n == 0) break;
        if (n < 0) { c->close_after_output = 1; return reply(c, "ERR Invalid or overlong command line\r\n"); }
        if (command_line(c, registry, queue, line)) return -1;
        progress = 1;
    }
    return 0;
}

static int set_nonblocking(int fd) { int flags = fcntl(fd, F_GETFL, 0); return flags < 0 ? -1 : fcntl(fd, F_SETFL, flags | O_NONBLOCK); }

int main(int argc, char **argv) {
    SmRegistry registry; SmIpc ipc; char error[256]; char *end; long port;
    struct sockaddr_in address; int listener, reuse = 1;
    if (argc != 3) { fprintf(stderr, "usage: %s <port> <userfile>\n", argv[0]); return 2; }
    errno = 0; port = strtol(argv[1], &end, 10);
    if (errno || *end || port < 1 || port > 65535) { fprintf(stderr, "smserver: invalid port\n"); return 2; }
    if (sm_registry_load(argv[2], &registry, error, sizeof(error))) { fprintf(stderr, "smserver: %s\n", error); return 1; }
    if (sm_ipc_join(&ipc, error, sizeof(error))) { fprintf(stderr, "smserver: %s\n", error); return 1; }
    listener = socket(AF_INET, SOCK_STREAM, 0); if (listener < 0) { perror("socket"); sm_ipc_close(&ipc); return 1; }
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    memset(&address, 0, sizeof(address)); address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_ANY); address.sin_port = htons((uint16_t)port);
    if (bind(listener, (struct sockaddr *)&address, sizeof(address)) || listen(listener, 64) || set_nonblocking(listener)) { perror("listen"); close(listener); sm_ipc_close(&ipc); return 1; }
    memset(clients, 0, sizeof(clients)); for (int i = 0; i < CLIENT_MAX; ++i) clients[i].fd = -1;
    pthread_mutex_lock(&ipc.queue->mutex); last_freed_generation = ipc.queue->freed_generation; pthread_mutex_unlock(&ipc.queue->mutex);
    signal(SIGINT, on_signal); signal(SIGTERM, on_signal); signal(SIGPIPE, SIG_IGN);
    sm_log("server", "listening on port %ld with %zu users", port, registry.count);
    while (!stopping) {
        struct pollfd fds[CLIENT_MAX + 1]; int map[CLIENT_MAX + 1], nfds = 1; fds[0].fd = listener; fds[0].events = POLLIN; map[0] = -1;
        for (int i = 0; i < CLIENT_MAX; ++i) if (clients[i].fd >= 0) {
            fds[nfds].fd = clients[i].fd; fds[nfds].events = POLLIN;
            if (clients[i].output_offset < clients[i].output_length) fds[nfds].events |= POLLOUT;
            map[nfds++] = i;
        }
        int result = poll(fds, (nfds_t)nfds, 500);
        if (result < 0 && errno != EINTR) { perror("poll"); break; }
        if (fds[0].revents & POLLIN) {
            for (;;) {
                int fd = accept(listener, NULL, NULL);
                if (fd < 0) { if (errno == EAGAIN || errno == EWOULDBLOCK) break; if (errno == EINTR) continue; break; }
                int slot = -1; for (int i = 0; i < CLIENT_MAX; ++i) if (clients[i].fd < 0) { slot = i; break; }
                if (slot < 0 || set_nonblocking(fd)) { close(fd); continue; }
                clients[slot].fd = fd; clients[slot].state = ST_MODE; clients[slot].accepted = time(NULL);
                reply(&clients[slot], "WELCOME SimpleMail v1.0\r\n"); sm_log("server", "client connected");
            }
        }
        for (int k = 1; k < nfds; ++k) {
            int i = map[k]; Client *c = &clients[i]; short ev = fds[k].revents;
            if (ev & (POLLERR | POLLHUP | POLLNVAL)) { drop_client(c); continue; }
            if (c->state == ST_MODE && time(NULL) - c->accepted >= 30) { c->close_after_output = 1; }
            if (ev & POLLIN) {
                if (c->input_length == sizeof(c->input)) { c->close_after_output = 1; }
                else {
                    ssize_t n = recv(c->fd, c->input + c->input_length, sizeof(c->input) - c->input_length, 0);
                    if (n <= 0) { drop_client(c); continue; }
                    c->input_length += (size_t)n;
                    if (process_input(c, &registry, ipc.queue)) c->close_after_output = 1;
                }
            }
            if ((ev & POLLOUT) || c->output_offset < c->output_length) {
                if (c->output_offset < c->output_length) {
                    ssize_t n = send(c->fd, c->output + c->output_offset, c->output_length - c->output_offset, MSG_NOSIGNAL);
                    if (n > 0) c->output_offset += (size_t)n;
                    else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) c->close_after_output = 1;
                }
            }
            if (c->close_after_output && c->output_offset == c->output_length) drop_client(c);
        }
        pthread_mutex_lock(&ipc.queue->mutex);
        uint64_t freed = ipc.queue->freed_generation;
        pthread_mutex_unlock(&ipc.queue->mutex);
        if (freed > last_freed_generation) {
            uint64_t slots = freed - last_freed_generation;
            for (int i = 0; i < CLIENT_MAX; ++i) {
                Client *c = &clients[i];
                if (c->fd >= 0 && c->state == ST_STREAM) {
                    size_t available = c->credits < SM_WINDOW_MAX ? SM_WINDOW_MAX - c->credits : 0;
                    uint64_t released = slots * (uint64_t)SM_INITIAL_WINDOW;
                    size_t grant = released > available ? available : (size_t)released;
                    if (grant) { c->credits += grant; reply(c, "WINDOW_UPDATE %zu\r\n", grant); }
                }
            }
            last_freed_generation = freed;
        }
        for (int i = 0; i < CLIENT_MAX; ++i) {
            Client *c = &clients[i]; uint64_t ticket;
            if (c->fd >= 0 && c->state == ST_WAIT_QUEUE && !sm_ipc_try_push(ipc.queue, &c->message, &ticket)) {
                sm_log("server", "queued ticket %llu from %s to %zu mailboxes", (unsigned long long)ticket, c->message.sender, c->message.recipient_count);
                reply(c, "OK Queued %llu to %zu mailboxes\r\n", (unsigned long long)ticket, c->message.recipient_count);
                memset(&c->message, 0, sizeof(c->message)); c->state = ST_FROM;
                if (process_input(c, &registry, ipc.queue)) c->close_after_output = 1;
            }
        }
    }
    for (int i = 0; i < CLIENT_MAX; ++i) if (clients[i].fd >= 0) { close(clients[i].fd); free(clients[i].output); }
    close(listener); sm_log("server", "stopping"); sm_ipc_close(&ipc); return 0;
}
