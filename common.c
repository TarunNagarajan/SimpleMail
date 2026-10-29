/* Group Details: Member 1 Name: Sumedh Jamadagni, Member 1 Roll number: 24CSB0A76, Member 2 Name: Tarun Nagarajan, Member 2 Roll number: 24CSB0A79 */
#define _POSIX_C_SOURCE 200809L
#include "common.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <time.h>
#include <unistd.h>

int sm_user_valid(const char *name) {
    size_t i, n = strlen(name);
    if (!n || n > SM_USER_MAX) return 0;
    for (i = 0; i < n; ++i)
        if (!((name[i] >= 'A' && name[i] <= 'Z') || (name[i] >= 'a' && name[i] <= 'z'))) return 0;
    return 1;
}

int sm_password_valid(const char *password) {
    size_t i, n = strlen(password);
    if (!n || n > SM_PASS_MAX) return 0;
    for (i = 0; i < n; ++i)
        if (!((password[i] >= 'A' && password[i] <= 'Z') ||
              (password[i] >= 'a' && password[i] <= 'z') ||
              (password[i] >= '0' && password[i] <= '9'))) return 0;
    return 1;
}

void sm_canonicalize(char *name) {
    for (; *name; ++name) if (*name >= 'A' && *name <= 'Z') *name = (char)(*name - 'A' + 'a');
}

int sm_user_find(const SmRegistry *registry, const char *name) {
    char key[SM_USER_MAX + 1];
    size_t i;
    if (strlen(name) > SM_USER_MAX) return -1;
    strcpy(key, name); sm_canonicalize(key);
    for (i = 0; i < registry->count; ++i) if (!strcmp(registry->users[i].name, key)) return (int)i;
    return -1;
}

int sm_registry_load(const char *path, SmRegistry *registry, char *error, size_t error_size) {
    FILE *file = fopen(path, "r");
    char line[256];
    size_t line_no = 0;
    memset(registry, 0, sizeof(*registry));
    if (!file) { snprintf(error, error_size, "cannot open registry '%s': %s", path, strerror(errno)); return -1; }
    while (fgets(line, sizeof(line), file)) {
        char user[SM_USER_MAX + 2], pass[SM_PASS_MAX + 2], extra;
        size_t n;
        ++line_no;
        n = strlen(line);
        if (n && line[n - 1] == '\n') line[--n] = '\0';
        if (n && line[n - 1] == '\r') line[--n] = '\0';
        if (!n || line[0] == '#') continue;
        if (sscanf(line, "%21s %31s %c", user, pass, &extra) != 2 ||
            !sm_user_valid(user) || !sm_password_valid(pass)) {
            snprintf(error, error_size, "invalid registry entry on line %zu", line_no); fclose(file); return -1;
        }
        if (registry->count >= SM_USER_COUNT) {
            snprintf(error, error_size, "registry exceeds %d users", SM_USER_COUNT); fclose(file); return -1;
        }
        sm_canonicalize(user);
        if (sm_user_find(registry, user) >= 0) {
            snprintf(error, error_size, "duplicate username on line %zu", line_no); fclose(file); return -1;
        }
        strcpy(registry->users[registry->count].name, user);
        strcpy(registry->users[registry->count].pass, pass);
        ++registry->count;
    }
    if (ferror(file) || !registry->count) {
        snprintf(error, error_size, "%s", ferror(file) ? "error reading registry" : "registry has no users");
        fclose(file); return -1;
    }
    fclose(file); return 0;
}

void sm_log(const char *component, const char *format, ...) {
    struct timespec now;
    struct tm tm;
    char stamp[32];
    va_list args;
    clock_gettime(CLOCK_REALTIME, &now);
    localtime_r(&now.tv_sec, &tm);
    strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &tm);
    (void)now.tv_nsec;
    fprintf(stdout, "[%s] %s: ", stamp, component);
    va_start(args, format); vfprintf(stdout, format, args); va_end(args);
    fputc('\n', stdout); fflush(stdout);
}

int sm_write_all(int fd, const void *data, size_t length) {
    const char *p = data;
    while (length) {
        ssize_t n = write(fd, p, length);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        p += n; length -= (size_t)n;
    }
    return 0;
}

int sm_read_line(int fd, char *buffer, size_t capacity, int timeout_seconds) {
    size_t used = 0;
    while (used + 1 < capacity) {
        char c;
        ssize_t n = read(fd, &c, 1);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        buffer[used++] = c;
        if (c == '\n') { buffer[used] = '\0'; return (int)used; }
        (void)timeout_seconds;
    }
    return -2;
}

unsigned long sm_djb2(const char *text) {
    unsigned long hash = 5381;
    const unsigned char *p = (const unsigned char *)text;
    while (*p) hash = ((hash << 5) + hash) + *p++;
    return hash;
}

int sm_constant_time_equal(const char *a, const char *b) {
    size_t na = strlen(a), nb = strlen(b), i, n = na > nb ? na : nb;
    unsigned char diff = (unsigned char)(na ^ nb);
    for (i = 0; i < n; ++i) diff |= (unsigned char)((i < na ? a[i] : 0) ^ (i < nb ? b[i] : 0));
    return diff == 0;
}

int sm_random_nonce(char nonce[9]) {
    static const char alphabet[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
    unsigned char bytes[8]; size_t i;
    ssize_t got = getrandom(bytes, sizeof(bytes), 0);
    if (got != (ssize_t)sizeof(bytes)) {
        int fd = open("/dev/urandom", O_RDONLY);
        size_t at = 0;
        if (fd < 0) return -1;
        while (at < sizeof(bytes)) {
            ssize_t n = read(fd, bytes + at, sizeof(bytes) - at);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) { close(fd); return -1; }
            at += (size_t)n;
        }
        close(fd);
    }
    for (i = 0; i < sizeof(bytes); ++i) nonce[i] = alphabet[bytes[i] % (sizeof(alphabet) - 1)];
    nonce[8] = '\0'; return 0;
}

void sm_dot_stuff(const char *input, size_t length, char **output, size_t *output_length) {
    size_t i, n = 0;
    char *result = malloc(length * 2 + 1);
    int at_line_start = 1;
    if (!result) { *output = NULL; *output_length = 0; return; }
    for (i = 0; i < length; ++i) {
        if (at_line_start && input[i] == '.') result[n++] = '.';
        result[n++] = input[i];
        at_line_start = input[i] == '\n';
    }
    *output = result; *output_length = n;
}
