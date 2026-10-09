#define _GNU_SOURCE
#include "check.h"

#include <errno.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* How long one case may take before it is reported as having hung. */
#define CASE_SECONDS 30

/* Shared with the parent, so the reason a child failed survives the child. */
static char *failure;
#define FAILURE_SIZE 1024

static char dir[256];

void check_fail(const char *format, ...) {
    if (failure[0] != '\0') {
        return;
    }
    va_list args;
    va_start(args, format);
    vsnprintf(failure, FAILURE_SIZE, format, args);
    va_end(args);
    for (char *c = failure; *c != '\0'; c++) {
        if (*c == '\n') {
            *c = ' ';
        }
    }
}

const char *check_dir(void) { return dir; }

long long check_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

void check_sleep(int ms) {
    struct timespec t = {.tv_sec = ms / 1000, .tv_nsec = (long)(ms % 1000) * 1000000};
    while (nanosleep(&t, &t) != 0 && errno == EINTR) {
    }
}

char *check_read(const char *path) {
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return NULL;
    }
    size_t size = 0, cap = 4096;
    char *out = malloc(cap);
    size_t n;
    while ((n = fread(out + size, 1, cap - size - 1, f)) > 0) {
        size += n;
        if (cap - size - 1 == 0) {
            cap *= 2;
            out = realloc(out, cap);
        }
    }
    fclose(f);
    out[size] = '\0';
    return out;
}

static void remove_tree(const char *path) {
    char command[512];
    snprintf(command, sizeof command, "rm -rf '%s'", path);
    if (system(command) != 0) {
        fprintf(stderr, "check: %s could not be removed\n", path);
    }
}

int check_run(const check_case *cases, size_t count) {
    failure = mmap(NULL, FAILURE_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (failure == MAP_FAILED) {
        perror("check: mmap");
        return 1;
    }
    int status = 0;
    for (size_t i = 0; i < count; i++) {
        failure[0] = '\0';
        // A short path: a Unix socket's name is bounded, and the cases bind several under it.
        snprintf(dir, sizeof dir, "/tmp/yoke-c-XXXXXX");
        if (mkdtemp(dir) == NULL) {
            perror("check: mkdtemp");
            return 1;
        }
        fflush(stdout);
        pid_t child = fork();
        if (child == 0) {
            alarm(CASE_SECONDS);
            cases[i].run();
            fflush(stdout);
            _exit(failure[0] == '\0' ? 0 : 1);
        }
        int waited;
        waitpid(child, &waited, 0);
        if (WIFSIGNALED(waited)) {
            const char *why =
                WTERMSIG(waited) == SIGALRM ? "it did not finish in time" : strsignal(WTERMSIG(waited));
            printf("FAIL  %s — %s%s%s\n", cases[i].id, why, failure[0] ? "; " : "", failure);
            status = 1;
        } else if (WEXITSTATUS(waited) != 0 || failure[0] != '\0') {
            printf("FAIL  %s — %s\n", cases[i].id, failure[0] ? failure : "it exited non-zero");
            status = 1;
        } else {
            printf("pass  %s\n", cases[i].id);
        }
        fflush(stdout);
        remove_tree(dir);
    }
    return status;
}
