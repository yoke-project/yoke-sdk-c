#include "harness.h"

static const yoke_stream streams[] = {
    {.id = "conformance.data"},
    {.id = "conformance.frames", .tolerates_loss = true},
    {0},
};
static const char *const commands[] = {"calibrate", NULL};
static const char *const queries[] = {"status", NULL};
static const char *const occurrences[] = {"conformance.drift", NULL};
static const yoke_capability capabilities[] = {
    {.name = "stream.data.publish", .governs = {YOKE_GOVERNS_STREAM, "conformance.data"}},
    {.name = "stream.frames.publish", .governs = {YOKE_GOVERNS_STREAM, "conformance.frames"}},
    {.name = "command.calibrate.accept", .governs = {YOKE_GOVERNS_COMMAND, "calibrate"}},
    {.name = "query.status.answer", .governs = {YOKE_GOVERNS_QUERY, "status"}},
    {.name = "event.drift.report", .governs = {YOKE_GOVERNS_OCCURRENCE, "conformance.drift"}},
    {0},
};
static const yoke_declaration declaration = {
    .id = "com.yoke.conformance.c",
    .streams = streams,
    .commands = commands,
    .queries = queries,
    .occurrences = occurrences,
    .capabilities = capabilities,
};

const yoke_declaration *harness_declaration(void) { return &declaration; }

int harness_serve(yoke_getenv getenv, void *context) {
    (void)getenv;
    (void)context;
    return 1;
}
