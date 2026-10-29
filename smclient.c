/*
Group Details:
Member 1: Sumedh Jamadagni, 24CSB0A76
Member 2: Tarun Nagarajan 24CSB0A79
*/
#define _POSIX_C_SOURCE 200809L
#include "common.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <unistd.h>

#define LINE 2048

static int read_server_line(int fd, char *line, size_t cap) {
    size_t used = 0;
    while (used + 1 < cap) {
        char c; ssize_t n = recv(fd, &c, 1, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        line[used++] = c;
        if (c == '\n') { line[used] = '\0'; return (int)used; }
    }
    return -1;
}

static int expect_line(int fd, char *line, size_t cap) {
    for (;;) {
        if (read_server_line(fd, line, cap) < 0) return -1;
        if (!strncmp(line, "WINDOW_UPDATE ", 14)) {
            printf("Server credit update: %s", line); continue;
        }
        fputs(line, stdout); fflush(stdout); return 0;
    }
}

static int send_command(int fd, const char *command) {
    size_t n = strlen(command); return sm_write_all(fd, command, n);
}

static int send_and_expect(int fd, const char *command, char *line, size_t cap) {
    if (send_command(fd, command)) return -1;
    return expect_line(fd, line, cap);
}

static void trim_newline(char *s) { s[strcspn(s, "\r\n")] = '\0'; }

static int prompt(const char *label, char *buffer, size_t cap) {
    printf("%s", label); fflush(stdout);
    if (!fgets(buffer, (int)cap, stdin)) return -1;
    trim_newline(buffer); return 0;
}

static int send_message(int fd) {
    char line[LINE], sender[SM_SENDER_MAX + 2], recipient[SM_USER_MAX + 2], subject[SM_SUBJECT_MAX + 2];
    unsigned char *body = malloc(SM_BODY_MAX + 1); size_t body_length = 0, credits; int rc = -1;
    if (!body) return -1;
    if (prompt("Sender display name: ", sender, sizeof(sender))) goto done;
    char command[LINE]; snprintf(command, sizeof(command), "FROM %s\r\n", sender);
    if (send_and_expect(fd, command, line, sizeof(line))) goto done;
    do {
        if (prompt("Recipient username (blank when finished): ", recipient, sizeof(recipient))) goto done;
        if (!recipient[0]) break;
        snprintf(command, sizeof(command), "TO %s\r\n", recipient);
        if (send_and_expect(fd, command, line, sizeof(line)) || !strncmp(line, "ERR", 3)) goto done;
    } while (1);
    if (prompt("Subject (blank for no subject): ", subject, sizeof(subject))) goto done;
    snprintf(command, sizeof(command), "SUB %s\r\n", subject);
    if (send_and_expect(fd, command, line, sizeof(line))) goto done;
    puts("Enter the body one line at a time. A line containing only a single period ends it.");
    while (body_length <= SM_BODY_MAX) {
        char text[LINE];
        if (!fgets(text, sizeof(text), stdin)) goto done;
        if (!strcmp(text, ".\n") || !strcmp(text, ".\r\n")) break;
        size_t n = strlen(text);
        for (size_t i = 0; i < n; ++i) {
            if (text[i] == '\n' && (i == 0 || text[i - 1] != '\r')) {
                if (body_length + 2 > SM_BODY_MAX) goto too_large;
                body[body_length++] = '\r'; body[body_length++] = '\n';
            } else {
                if (body_length == SM_BODY_MAX) goto too_large;
                body[body_length++] = (unsigned char)text[i];
            }
        }
    }
    if (body_length > SM_BODY_MAX) goto too_large;
    if (send_and_expect(fd, "BODY_STREAM\r\n", line, sizeof(line))) goto done;
    if (sscanf(line, "OK Window %zu", &credits) != 1) goto done;
    size_t offset = 0;
    while (offset < body_length) {
        while (!credits) {
            if (read_server_line(fd, line, sizeof(line)) < 0) goto done;
            size_t added;
            if (sscanf(line, "WINDOW_UPDATE %zu", &added) == 1) credits += added;
            else { fputs(line, stdout); goto done; }
        }
        size_t n = body_length - offset; if (n > 4096) n = 4096; if (n > credits) n = credits;
        snprintf(command, sizeof(command), "CHUNK %zu\r\n", n);
        if (send_command(fd, command) || sm_write_all(fd, body + offset, n) || expect_line(fd, line, sizeof(line))) goto done;
        size_t remaining; if (sscanf(line, "OK Window %zu", &remaining) != 1) goto done;
        credits = remaining; offset += n;
    }
    if (send_and_expect(fd, "END_BODY\r\n", line, sizeof(line))) goto done;
    rc = !strncmp(line, "OK Queued ", 10) ? 0 : -1; goto done;
too_large:
    fputs("Body exceeds 65536 bytes.\n", stderr);
done:
    free(body); return rc;
}

static int receive_mail(int fd) {
    char username[SM_USER_MAX + 2], password[SM_PASS_MAX + 2], line[LINE], nonce[32], payload[64], command[LINE];
    if (read_server_line(fd, line, sizeof(line)) < 0) return -1;
    if (!strcmp(line, "OK\r\n") && read_server_line(fd, line, sizeof(line)) < 0) return -1;
    if (sscanf(line, "AUTH REQUIRED %31s", nonce) != 1) { fputs(line, stdout); return -1; }
    printf("Mailbox nonce: %s\n", nonce);
    if (prompt("Username: ", username, sizeof(username)) || prompt("Password: ", password, sizeof(password))) return -1;
    snprintf(payload, sizeof(payload), "%s%s", password, nonce);
    snprintf(command, sizeof(command), "AUTH %s %lu\r\n", username, sm_djb2(payload));
    if (send_command(fd, command) || expect_line(fd, line, sizeof(line)) || strncmp(line, "OK Welcome ", 11)) return -1;
    for (;;) {
        char choice[16];
        puts("Mailbox: [L]ist, [R]ead, [D]elete, [C]ount, [Q]uit");
        if (prompt("Choice: ", choice, sizeof(choice))) return -1;
        if (!strcasecmp(choice, "L")) {
            if (send_and_expect(fd, "LIST\r\n", line, sizeof(line))) return -1;
            while (strcmp(line, ".\r\n")) { if (read_server_line(fd, line, sizeof(line)) < 0) return -1; fputs(line, stdout); }
        } else if (!strcasecmp(choice, "R")) {
            char id[32]; if (prompt("Message ID: ", id, sizeof(id))) return -1;
            snprintf(command, sizeof(command), "READ %s\r\n", id);
            if (send_command(fd, command) || read_server_line(fd, line, sizeof(line)) < 0) return -1;
            fputs(line, stdout);
            if (!strcmp(line, "OK\r\n")) {
                for (;;) {
                    if (read_server_line(fd, line, sizeof(line)) < 0) return -1;
                    if (!strcmp(line, ".\r\n")) break;
                    fputs(line + (line[0] == '.' && line[1] == '.'), stdout);
                }
            }
        } else if (!strcasecmp(choice, "D")) {
            char id[32]; if (prompt("Message ID: ", id, sizeof(id))) return -1;
            snprintf(command, sizeof(command), "DELETE %s\r\n", id);
            if (send_and_expect(fd, command, line, sizeof(line))) return -1;
        } else if (!strcasecmp(choice, "C")) {
            if (send_and_expect(fd, "COUNT\r\n", line, sizeof(line))) return -1;
        } else if (!strcasecmp(choice, "Q")) {
            if (send_and_expect(fd, "QUIT\r\n", line, sizeof(line))) return -1;
            return 1;
        }
    }
}

static int connect_to(const char *host, const char *service) {
    struct addrinfo hints, *list, *it; int fd = -1;
    memset(&hints, 0, sizeof(hints)); hints.ai_family = AF_UNSPEC; hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, service, &hints, &list)) return -1;
    for (it = list; it; it = it->ai_next) {
        fd = socket(it->ai_family, it->ai_socktype, it->ai_protocol);
        if (fd >= 0 && connect(fd, it->ai_addr, it->ai_addrlen) == 0) break;
        if (fd >= 0) close(fd);
        fd = -1;
    }
    freeaddrinfo(list); return fd;
}

int main(int argc, char **argv) {
    char line[LINE];
    if (argc != 3) { fprintf(stderr, "usage: %s <server_ip> <port>\n", argv[0]); return 2; }
    for (;;) {
        char choice[16];
        puts("1. Send a mail\n2. Check my mailbox\n3. Quit");
        if (prompt("Choice: ", choice, sizeof(choice))) break;
        if ((!strcmp(choice, "3") || !strcasecmp(choice, "Q"))) break;
        int fd = connect_to(argv[1], argv[2]);
        if (fd < 0) { perror("connect"); continue; }
        if (read_server_line(fd, line, sizeof(line)) < 0) { close(fd); continue; }
        fputs(line, stdout);
        if ((!strcmp(choice, "1") || !strcasecmp(choice, "S"))) {
            if (send_command(fd, "MODE SEND\r\n") || expect_line(fd, line, sizeof(line)) || send_message(fd)) {
                close(fd); continue;
            }
            close(fd);
        } else if ((!strcmp(choice, "2") || !strcasecmp(choice, "M"))) {
            if (send_command(fd, "MODE RECV\r\n")) { close(fd); continue; }
            (void)receive_mail(fd); close(fd);
        } else close(fd);
    }
    return 0;
}
