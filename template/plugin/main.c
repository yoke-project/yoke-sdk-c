/* @YOKE_PROGRAM@: a Yoke Plugin. Its Manifest is kept beside this file, as @YOKE_ID@.yaml, and is what
 * `@YOKE_PROGRAM@ --manifest` prints; the continuous integration of this project compares the two. */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <yoke/plugin.h>

/* What this Plugin says about itself: its identity, and whatever it publishes and answers. */
static const yoke_declaration declaration = {
    .id = "@YOKE_ID@",
};

static volatile sig_atomic_t stopping;
static void on_term(int sig) {
    (void)sig;
    stopping = 1;
}

int main(int argc, char **argv) {
    if (argc == 2 && strcmp(argv[1], "--manifest") == 0) {
        char *manifest = yoke_manifest(&declaration);
        fputs(manifest, stdout);
        free(manifest);
        return 0;
    }
    signal(SIGTERM, on_term);
    yoke_error err = {0};
    yoke_unit *unit = yoke_start(&declaration, &err);
    if (unit == NULL) {
        fprintf(stderr, "@YOKE_PROGRAM@: %s\n", err.message);
        yoke_error_clear(&err);
        return 1;
    }
    yoke_health(unit, 0, "ready", &err);
    yoke_event event = {0};
    while (!stopping) {
        int got = yoke_next_within(unit, &event, 200);
        if (got == 0)
            break; /* the Session has ended */
        if (got == 1)
            yoke_event_clear(&event);
    }
    if (stopping) {
        yoke_close(unit, &err);
        while (yoke_next(unit, &event) == 1)
            yoke_event_clear(&event);
    }
    yoke_free(unit);
    return 0;
}
