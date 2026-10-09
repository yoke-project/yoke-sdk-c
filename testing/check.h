/*
 * The cases of a test program, each run in a process of its own and reported by the identifier its
 * marker names, in the form the record writer reads: `pass  <id>`, or `FAIL  <id> — <why>`.
 */
#ifndef YOKE_CHECK_H
#define YOKE_CHECK_H

#include <stdbool.h>
#include <stddef.h>

typedef struct check_case {
    const char *id;
    void (*run)(void);
} check_case;

/* Records why the running case failed; the first reason is the one reported. */
void check_fail(const char *format, ...) __attribute__((format(printf, 1, 2)));

/* Fails the case, and jumps to its `done:` label, unless cond holds. */
#define REQUIRE(cond, ...)                                                                                   \
    do {                                                                                                     \
        if (!(cond)) {                                                                                       \
            check_fail(__VA_ARGS__);                                                                         \
            goto done;                                                                                       \
        }                                                                                                    \
    } while (0)

/* Runs every case, each in a child process with a bound on its time; 0 when all passed. */
int check_run(const check_case *cases, size_t count);

/* A directory of the case's own, removed when the program ends. */
const char *check_dir(void);

/* Milliseconds on a monotonic clock. */
long long check_ms(void);

/* Waits ms milliseconds. */
void check_sleep(int ms);

/* The whole of a file, in a string the caller frees; NULL when it cannot be read. */
char *check_read(const char *path);

#endif
