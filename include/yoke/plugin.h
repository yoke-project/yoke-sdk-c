/*
 * The library a Plugin unit is written with.
 *
 * One declaration is both the Manifest the library generates and the surface its registration claims,
 * so the two cannot be written apart. Starting a unit performs the first acts in their order: read the
 * environment, bind the unit's own socket, register, open the Session — and then beats on the terms the
 * Core assigned, repeating the author's last health report. Everything the Session brings is handed to
 * the author as an event, its end included: a Session that ends ends the incarnation, and the library
 * never reconnects, never retries an admission, never polls, never creates a stream's transport and
 * never chooses a severity or a grade.
 *
 * The library runs two threads of its own, one reading the Session and one beating. Every function
 * below may be called from any thread.
 */
#ifndef YOKE_PLUGIN_H
#define YOKE_PLUGIN_H

#include <stddef.h>

#include "yoke/base.h"

#ifdef __cplusplus
extern "C" {
#endif

/* A stream the Plugin may publish, and what its data tolerates. */
typedef struct yoke_stream {
    const char *id;
    bool tolerates_loss;
    bool tolerates_reorder;
} yoke_stream;

/* The kind of the one thing a capability governs. */
typedef enum yoke_object_kind {
    YOKE_GOVERNS_STREAM = 1,
    YOKE_GOVERNS_COMMAND,
    YOKE_GOVERNS_QUERY,
    YOKE_GOVERNS_OCCURRENCE,
    YOKE_GOVERNS_SURFACE,
} yoke_object_kind;

/* The one thing a capability governs. */
typedef struct yoke_object {
    yoke_object_kind kind;
    const char *id;
} yoke_object;

/* What an operator may grant, governing exactly one object. */
typedef struct yoke_capability {
    const char *name;
    yoke_object governs;
} yoke_capability;

/*
 * What a Plugin says about itself: what is true of the binary wherever it runs. Every list ends with a
 * NULL — a stream with a NULL id, a capability with a NULL name — and a NULL list is an empty one. The
 * declaration is read, never kept: the library copies what it needs.
 */
typedef struct yoke_declaration {
    const char *id;
    const char *const *needs; /* `<class>` or `<class>:<name>` */
    const yoke_stream *streams;
    const char *const *commands;
    const char *const *queries;
    const char *const *occurrences;
    const yoke_capability *capabilities;
} yoke_declaration;

/* The document the declaration generates, in a string the caller frees; NULL if memory ran out. */
char *yoke_manifest(const yoke_declaration *declaration);

/* Five lists, each ending with a NULL: what was granted, or what was withheld. */
typedef struct yoke_scope {
    char **capabilities;
    char **streams;
    char **commands;
    char **queries;
    char **occurrences;
} yoke_scope;

/* What the Core answered the registration with. */
typedef struct yoke_admission {
    bool restricted;
    yoke_scope granted;
    yoke_scope withheld; /* item by item */
} yoke_admission;

/* A started unit and its Session. */
typedef struct yoke_unit yoke_unit;

/* Starts a unit from the process's environment. */
yoke_unit *yoke_start(const yoke_declaration *declaration, yoke_error *err);

/*
 * Starts a unit from the environment getenv reads: bind, register, open the Session. A refusal is
 * returned with its stage and code, and nothing is tried again. NULL when it did not start.
 */
yoke_unit *yoke_start_with(const yoke_declaration *declaration, yoke_getenv getenv, void *context,
                           yoke_error *err);

/* What the Core answered the registration with; owned by the unit. */
const yoke_admission *yoke_admission_of(const yoke_unit *unit);

/* What the Session brings. */
typedef enum yoke_event_kind {
    YOKE_COMMAND = 1, /* an instruction the Core sent, to be acknowledged */
    YOKE_QUESTION,    /* a query the Core asked, to be answered */
    YOKE_ACTIVATED,   /* a stream may now be emitted on, and where */
    YOKE_STOPPED,     /* a stream may no longer be emitted on */
    YOKE_REFUSED,     /* an error the Core answered a message with */
    YOKE_ENDED,       /* the end of the Session: closed by this unit, or revoked. Nothing follows it */
} yoke_event_kind;

/* One event. Every field the kind does not name is empty; every string is owned by the event. */
typedef struct yoke_event {
    yoke_event_kind kind;
    char *id;   /* command, question: the message identity an answer is correlated to */
    char *type; /* command, question */
    unsigned char *payload;
    size_t payload_len;
    char *stream;      /* activated, stopped */
    char *transport;   /* activated: `ordered` or `framed` */
    char *address;     /* activated */
    char *correlation; /* refused: the identity of the message the Core refused */
    yoke_error error;  /* refused */
    bool closed;       /* ended: by this unit's close */
    char *cause;       /* ended by a revocation: liveness lost, plugin disabled, scope exceeded, protocol
                          failure */
    char *line;        /* ended */
} yoke_event;

/* Releases what an event holds. */
void yoke_event_clear(yoke_event *event);

/*
 * The next event, in the order the Session brought them, waiting for one. Returns 1 with an event,
 * and 0 once the end has been handed over: nothing follows it.
 */
int yoke_next(yoke_unit *unit, yoke_event *event);

/* As yoke_next, waiting at most timeout_ms: returns -1 when that passed with no event. */
int yoke_next_within(yoke_unit *unit, yoke_event *event, int timeout_ms);

/* Whether the Session has ended. The incarnation is over: the process should finish. */
bool yoke_done(yoke_unit *unit);

/* Ends the Session in order: a CLOSE, the unit's own departure. */
int yoke_close(yoke_unit *unit, yoke_error *err);

/* Releases the unit, ending its Session first if it has not ended. */
void yoke_free(yoke_unit *unit);

/* What became of a command. */
typedef enum yoke_outcome {
    YOKE_ACCEPTED = 1,
    YOKE_DONE,
    YOKE_FAILED,
} yoke_outcome;

/* Says what became of a command, correlated to it. */
int yoke_ack(yoke_unit *unit, const yoke_event *command, yoke_outcome outcome, const char *line,
             yoke_error *err);

/* Answers a question, correlated to it. */
int yoke_answer(yoke_unit *unit, const yoke_event *question, const void *payload, size_t len,
                yoke_error *err);

/* Says what went wrong with a message the Core sent, correlated to it: a code and a message. */
int yoke_fail(yoke_unit *unit, const char *about, const char *code, const char *message, yoke_error *err);

/*
 * How routine or alarming an occurrence is, from 0 to 99: the author's statement and nobody else's.
 * A zeroed severity states nothing, and an occurrence reported with it is refused.
 */
typedef struct yoke_severity {
    unsigned value;
    bool set;
} yoke_severity;

/* A severity the author states. */
#define YOKE_SEVERITY(n) ((yoke_severity){.value = (n), .set = true})

/* Reports an occurrence of a declared class, with the author's severity. */
int yoke_report(yoke_unit *unit, const char *occurrence, yoke_severity severity, const char *line,
                const void *detail, size_t len, yoke_error *err);

/*
 * Reports how well the unit is: a grade from 0 to 99, and a line. The library repeats the last report
 * at every beat, and sends no beat before the first: a unit keeps its liveness only once its author
 * has reported, so the first report must come within the tolerance the Core assigned.
 */
int yoke_health(yoke_unit *unit, unsigned grade, const char *line, yoke_error *err);

/*
 * Sends data on a stream, on the transport its activation named: one data envelope per packet on the
 * ordered transport, one frame per datagram on the framed one, numbered from 1 within the activation.
 * Only the Core creates a stream's transport, so a stream it has not activated has nowhere to be
 * written, and the library refuses rather than make one.
 */
int yoke_emit(yoke_unit *unit, const char *stream, const void *payload, size_t len, yoke_error *err);

#ifdef __cplusplus
}
#endif

#endif
