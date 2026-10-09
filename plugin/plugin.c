/*
 * The plugin library. One thread reads the Session's completion queue and turns what arrives into
 * events; a second beats and keeps the deadline of a close. Writes wait in order and go one at a time,
 * which is all gRPC allows on a call, so no function that sends ever waits for the Core.
 */
#include "yoke/plugin.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include <grpc/support/alloc.h>

#include "wire.h"
#include "yoke/plugin/v1/register.pb-c.h"

/* How long a close waits for the Core to end the stream before the departure is one anyway. */
#define CLOSE_GRACE_NS 2000000000LL

/* What a completion on the Session's queue is about. */
typedef enum { SENT_METADATA, READ, WRITTEN, STATUS, KINDS } kind;

typedef struct tag {
    kind kind;
} tag;

/* One write the Session waits to make: a message, or the end of this side of the stream. */
typedef struct pending {
    grpc_byte_buffer *message; /* NULL: the half-close */
    struct pending *next;
} pending;

typedef struct queued {
    yoke_event event;
    struct queued *next;
} queued;

/* One activated stream: the library's connection to its transport, and the next sequence. */
typedef struct flow {
    char *stream;
    int fd;
    bool framed;
    pthread_mutex_t mu;
    unsigned long long next;
    int refs; /* the unit's list holds one, and every emission in progress one */
    struct flow *link;
} flow;

struct yoke_unit {
    yoke_admission admission;
    grpc_channel *channel;
    grpc_completion_queue *cq;
    grpc_call *call;
    int listener;
    char *bind;
    yoke_envelopes envelopes;
    long long interval_ns;
    tag tags[KINDS];
    pthread_t reader, beater;

    pthread_mutex_t mu;
    pthread_cond_t changed;

    queued *head, *tail;
    bool handed_end;

    pending *writes, *last;
    bool writing;
    grpc_byte_buffer *in_flight; /* kept until its write completes, as gRPC requires */

    grpc_byte_buffer *incoming;
    grpc_metadata_array initial, trailing;
    grpc_status_code status;
    grpc_slice details;
    int outstanding; /* batches started on the call and not yet completed */
    bool status_in;

    bool ended, closing;
    long long close_by;
    flow *active;

    /*
     * The author's last health report, which every beat repeats; none until one. Held while a report
     * is read and sent, so a beat never sends a report older than one the author has already sent.
     */
    pthread_mutex_t reporting;
    bool reported;
    unsigned grade;
    char *line;
};

/* ---- the Manifest ---- */

static void quoted(FILE *out, const char *s) {
    fputc('"', out);
    for (const unsigned char *c = (const unsigned char *)s; *c != '\0'; c++) {
        if (*c == '"' || *c == '\\') {
            fprintf(out, "\\%c", *c);
        } else if (*c < 0x20 || *c == 0x7f) {
            fprintf(out, "\\x%02x", *c);
        } else {
            fputc(*c, out);
        }
    }
    fputc('"', out);
}

static void ids(FILE *out, const char *key, const char *const *list) {
    if (list == NULL || list[0] == NULL) {
        return;
    }
    fprintf(out, "%s:\n", key);
    for (size_t i = 0; list[i] != NULL; i++) {
        fputs("  - id: ", out);
        quoted(out, list[i]);
        fputc('\n', out);
    }
}

static const char *governed(yoke_object_kind kind) {
    switch (kind) {
    case YOKE_GOVERNS_STREAM:
        return "stream";
    case YOKE_GOVERNS_COMMAND:
        return "command";
    case YOKE_GOVERNS_QUERY:
        return "query";
    case YOKE_GOVERNS_OCCURRENCE:
        return "occurrence";
    case YOKE_GOVERNS_SURFACE:
        return "surface";
    }
    return NULL;
}

char *yoke_manifest(const yoke_declaration *d) {
    char *text = NULL;
    size_t size = 0;
    FILE *out = open_memstream(&text, &size);
    if (out == NULL) {
        return NULL;
    }
    fputs("manifest: 1\nid: ", out);
    quoted(out, d->id ? d->id : "");
    fprintf(out, "\nprotocol: %d\n", yoke_plugin_contract());
    if (d->needs != NULL && d->needs[0] != NULL) {
        fputs("needs:\n", out);
        for (size_t i = 0; d->needs[i] != NULL; i++) {
            fputs("  - ", out);
            quoted(out, d->needs[i]);
            fputc('\n', out);
        }
    }
    if (d->streams != NULL && d->streams[0].id != NULL) {
        fputs("streams:\n", out);
        for (const yoke_stream *s = d->streams; s->id != NULL; s++) {
            fputs("  - id: ", out);
            quoted(out, s->id);
            fputc('\n', out);
            if (s->tolerates_loss) {
                fputs("    tolerates_loss: true\n", out);
            }
            if (s->tolerates_reorder) {
                fputs("    tolerates_reorder: true\n", out);
            }
        }
    }
    ids(out, "commands", d->commands);
    ids(out, "queries", d->queries);
    ids(out, "occurrences", d->occurrences);
    if (d->capabilities != NULL && d->capabilities[0].name != NULL) {
        fputs("capabilities:\n", out);
        for (const yoke_capability *c = d->capabilities; c->name != NULL; c++) {
            fputs("  - name: ", out);
            quoted(out, c->name);
            fputs("\n    governs:\n", out);
            const char *what = governed(c->governs.kind);
            if (what != NULL) {
                fprintf(out, "      %s: ", what);
                quoted(out, c->governs.id ? c->governs.id : "");
                fputc('\n', out);
            }
        }
    }
    if (fclose(out) != 0) {
        free(text);
        return NULL;
    }
    return text;
}

/* ---- the declaration, as the registration claims it ---- */

static size_t count(const char *const *list) {
    size_t n = 0;
    while (list != NULL && list[n] != NULL) {
        n++;
    }
    return n;
}

/* The surface the registration claims: the declaration again, from the same value. */
static void claim(const yoke_declaration *d, Yoke__Plugin__V1__Surface *s, char ***owned) {
    size_t streams = 0, capabilities = 0;
    while (d->streams != NULL && d->streams[streams].id != NULL) {
        streams++;
    }
    while (d->capabilities != NULL && d->capabilities[capabilities].name != NULL) {
        capabilities++;
    }
    owned[0] = calloc(streams + 1, sizeof(char *));
    owned[1] = calloc(capabilities + 1, sizeof(char *));
    for (size_t i = 0; i < streams; i++) {
        owned[0][i] = (char *)d->streams[i].id;
    }
    for (size_t i = 0; i < capabilities; i++) {
        owned[1][i] = (char *)d->capabilities[i].name;
    }
    s->n_streams = streams;
    s->streams = owned[0];
    s->n_capabilities = capabilities;
    s->capabilities = owned[1];
    s->n_commands = count(d->commands);
    s->commands = (char **)d->commands;
    s->n_queries = count(d->queries);
    s->queries = (char **)d->queries;
    s->n_occurrences = count(d->occurrences);
    s->occurrences = (char **)d->occurrences;
}

static char **copy_list(char **list, size_t n) {
    char **out = calloc(n + 1, sizeof(char *));
    for (size_t i = 0; i < n; i++) {
        out[i] = strdup(list[i]);
    }
    return out;
}

static void scope_of(const Yoke__Plugin__V1__Surface *s, yoke_scope *scope) {
    static Yoke__Plugin__V1__Surface none = YOKE__PLUGIN__V1__SURFACE__INIT;
    if (s == NULL) {
        s = &none;
    }
    scope->capabilities = copy_list(s->capabilities, s->n_capabilities);
    scope->streams = copy_list(s->streams, s->n_streams);
    scope->commands = copy_list(s->commands, s->n_commands);
    scope->queries = copy_list(s->queries, s->n_queries);
    scope->occurrences = copy_list(s->occurrences, s->n_occurrences);
}

static void free_list(char **list) {
    for (size_t i = 0; list != NULL && list[i] != NULL; i++) {
        free(list[i]);
    }
    free(list);
}

static void scope_clear(yoke_scope *scope) {
    free_list(scope->capabilities);
    free_list(scope->streams);
    free_list(scope->commands);
    free_list(scope->queries);
    free_list(scope->occurrences);
}

/* An enumeration's value as its name says it, without its prefix: `STAGE_UNIT_CONFLICT`, `unit conflict`. */
static char *spoken(const ProtobufCEnumDescriptor *descriptor, int value, const char *prefix) {
    const ProtobufCEnumValue *v = protobuf_c_enum_descriptor_get_value(descriptor, value);
    if (v == NULL) {
        return strdup("unspecified");
    }
    const char *name = v->name;
    if (strncmp(name, prefix, strlen(prefix)) == 0) {
        name += strlen(prefix);
    }
    char *out = strdup(name);
    for (char *c = out; *c != '\0'; c++) {
        *c = *c == '_' ? ' ' : (char)tolower((unsigned char)*c);
    }
    return out;
}

/* ---- events ---- */

void yoke_event_clear(yoke_event *event) {
    if (event == NULL) {
        return;
    }
    free(event->id);
    free(event->type);
    free(event->payload);
    free(event->stream);
    free(event->transport);
    free(event->address);
    free(event->correlation);
    yoke_error_clear(&event->error);
    free(event->cause);
    free(event->line);
    memset(event, 0, sizeof *event);
}

/* Hands an event over. Holds mu. */
static void push(yoke_unit *u, const yoke_event *event) {
    queued *q = calloc(1, sizeof *q);
    q->event = *event;
    if (u->tail != NULL) {
        u->tail->next = q;
    } else {
        u->head = q;
    }
    u->tail = q;
    pthread_cond_broadcast(&u->changed);
}

static void push_now(yoke_unit *u, const yoke_event *event) {
    pthread_mutex_lock(&u->mu);
    push(u, event);
    pthread_mutex_unlock(&u->mu);
}

static struct timespec deadline_in(int ms) {
    struct timespec t;
    clock_gettime(CLOCK_REALTIME, &t);
    t.tv_sec += ms / 1000;
    t.tv_nsec += (long)(ms % 1000) * 1000000;
    if (t.tv_nsec >= 1000000000) {
        t.tv_sec++;
        t.tv_nsec -= 1000000000;
    }
    return t;
}

static int next_event(yoke_unit *u, yoke_event *event, int timeout_ms) {
    struct timespec deadline = deadline_in(timeout_ms < 0 ? 0 : timeout_ms);
    pthread_mutex_lock(&u->mu);
    while (u->head == NULL) {
        if (u->handed_end) {
            pthread_mutex_unlock(&u->mu);
            return 0;
        }
        if (timeout_ms < 0) {
            pthread_cond_wait(&u->changed, &u->mu);
        } else if (pthread_cond_timedwait(&u->changed, &u->mu, &deadline) == ETIMEDOUT) {
            pthread_mutex_unlock(&u->mu);
            return -1;
        }
    }
    queued *q = u->head;
    u->head = q->next;
    if (u->head == NULL) {
        u->tail = NULL;
    }
    if (q->event.kind == YOKE_ENDED) {
        u->handed_end = true;
    }
    pthread_mutex_unlock(&u->mu);
    *event = q->event;
    free(q);
    return 1;
}

int yoke_next(yoke_unit *u, yoke_event *event) { return next_event(u, event, -1); }

int yoke_next_within(yoke_unit *u, yoke_event *event, int timeout_ms) {
    return next_event(u, event, timeout_ms < 0 ? 0 : timeout_ms);
}

bool yoke_done(yoke_unit *u) {
    pthread_mutex_lock(&u->mu);
    bool ended = u->ended;
    pthread_mutex_unlock(&u->mu);
    return ended;
}

const yoke_admission *yoke_admission_of(const yoke_unit *u) { return &u->admission; }

/* ---- writing ---- */

/* Starts the next write, if there is one and none is outstanding. Holds mu. */
static void start_write(yoke_unit *u) {
    if (u->writing || u->writes == NULL || u->status_in) {
        return;
    }
    pending *w = u->writes;
    u->writes = w->next;
    if (u->writes == NULL) {
        u->last = NULL;
    }
    grpc_op op = {0};
    if (w->message != NULL) {
        op.op = GRPC_OP_SEND_MESSAGE;
        op.data.send_message.send_message = w->message;
    } else {
        op.op = GRPC_OP_SEND_CLOSE_FROM_CLIENT;
    }
    u->writing = true;
    u->in_flight = w->message;
    u->outstanding++;
    grpc_call_start_batch(u->call, &op, 1, &u->tags[WRITTEN], NULL);
    free(w);
}

/* Queues a write. Holds mu. */
static void queue_write(yoke_unit *u, grpc_byte_buffer *message) {
    pending *w = calloc(1, sizeof *w);
    w->message = message;
    if (u->last != NULL) {
        u->last->next = w;
    } else {
        u->writes = w;
    }
    u->last = w;
    start_write(u);
}

/* Seals e — as an answer to `to`, where there is one — and sends it on the Session. */
static int send_envelope(yoke_unit *u, const char *to, Yoke__Plugin__V1__Envelope *e, yoke_error *err) {
    char id[YOKE_ID_SIZE];
    pthread_mutex_lock(&u->mu);
    if (u->ended) {
        pthread_mutex_unlock(&u->mu);
        yoke_error_refuse(err, "session.revoked", "the Session has ended");
        return -1;
    }
    if (to != NULL) {
        yoke_answer_to(&u->envelopes, to, e, id);
    } else {
        yoke_seal(&u->envelopes, e, id);
    }
    queue_write(u, yoke_pack(&e->base));
    pthread_mutex_unlock(&u->mu);
    return 0;
}

/* ---- streams ---- */

static void release(yoke_unit *u, flow *f) {
    pthread_mutex_lock(&u->mu);
    bool last = --f->refs == 0;
    pthread_mutex_unlock(&u->mu);
    if (last) {
        close(f->fd);
        pthread_mutex_destroy(&f->mu);
        free(f->stream);
        free(f);
    }
}

/* Takes a flow out of the list and closes its connection; the last reference frees it. Holds mu. */
static flow *unlink_flow(yoke_unit *u, const char *stream) {
    for (flow **at = &u->active; *at != NULL; at = &(*at)->link) {
        if (strcmp((*at)->stream, stream) == 0) {
            flow *f = *at;
            *at = f->link;
            shutdown(f->fd, SHUT_RDWR);
            return f;
        }
    }
    return NULL;
}

/* Reaches the transport an activation names, as the Core created it; 0, or -1 with the reason. */
static int connect_flow(const Yoke__Plugin__V1__Control__Activate *a, flow **out, char **why) {
    const char *name = NULL;
    int type = 0;
    switch (a->transport) {
    case YOKE__PLUGIN__V1__CONTROL__ACTIVATE__TRANSPORT__TRANSPORT_ORDERED:
        name = "ordered";
        type = SOCK_SEQPACKET;
        break;
    case YOKE__PLUGIN__V1__CONTROL__ACTIVATE__TRANSPORT__TRANSPORT_FRAMED:
        name = "framed";
        type = SOCK_DGRAM;
        break;
    default: {
        char *transport = spoken(&yoke__plugin__v1__control__activate__transport__descriptor,
                                 (int)a->transport, "TRANSPORT_");
        if (asprintf(why, "the transport %s of %s is not one this library reaches", transport, a->stream) <
            0) {
            *why = NULL;
        }
        free(transport);
        return -1;
    }
    }
    struct sockaddr_un addr = {.sun_family = AF_UNIX};
    if (strlen(a->address) >= sizeof addr.sun_path) {
        if (asprintf(why, "the transport of %s at %s cannot be reached: the path is too long", a->stream,
                     a->address) < 0) {
            *why = NULL;
        }
        return -1;
    }
    strcpy(addr.sun_path, a->address);
    int fd = socket(AF_UNIX, type | SOCK_CLOEXEC, 0);
    if (fd < 0 || connect(fd, (struct sockaddr *)&addr, sizeof addr) != 0) {
        int code = errno;
        if (fd >= 0) {
            close(fd);
        }
        if (asprintf(why, "the transport of %s at %s cannot be reached: %s", a->stream, a->address,
                     strerror(code)) < 0) {
            *why = NULL;
        }
        return -1;
    }
    flow *f = calloc(1, sizeof *f);
    f->stream = strdup(a->stream);
    f->fd = fd;
    f->framed = type == SOCK_DGRAM;
    f->refs = 1;
    pthread_mutex_init(&f->mu, NULL);
    *out = f;
    (void)name;
    return 0;
}

static const char *transport_name(Yoke__Plugin__V1__Control__Activate__Transport t) {
    return t == YOKE__PLUGIN__V1__CONTROL__ACTIVATE__TRANSPORT__TRANSPORT_FRAMED ? "framed" : "ordered";
}

static void acknowledge(yoke_unit *u, const char *to, Yoke__Plugin__V1__Ack__Outcome outcome,
                        const char *line) {
    Yoke__Plugin__V1__Ack ack = YOKE__PLUGIN__V1__ACK__INIT;
    ack.outcome = outcome;
    ack.line = (char *)(line ? line : "");
    Yoke__Plugin__V1__Envelope e = YOKE__PLUGIN__V1__ENVELOPE__INIT;
    e.payload_case = YOKE__PLUGIN__V1__ENVELOPE__PAYLOAD_ACK;
    e.ack = &ack;
    send_envelope(u, to, &e, NULL);
}

int yoke_emit(yoke_unit *u, const char *stream, const void *payload, size_t len, yoke_error *err) {
    pthread_mutex_lock(&u->mu);
    flow *f = u->active;
    while (f != NULL && strcmp(f->stream, stream) != 0) {
        f = f->link;
    }
    if (f != NULL) {
        f->refs++;
    }
    pthread_mutex_unlock(&u->mu);
    if (f == NULL) {
        yoke_error_refuse(err, "stream.inactive", "the stream %s has not been activated", stream);
        return -1;
    }
    int result = 0;
    pthread_mutex_lock(&f->mu);
    unsigned long long sequence = ++f->next;
    unsigned char *message = NULL;
    size_t size = 0;
    if (f->framed) {
        size = 16 + len;
        message = malloc(size);
        unsigned long long clock = (unsigned long long)yoke_now();
        for (int i = 0; i < 8; i++) {
            message[i] = (unsigned char)(sequence >> (8 * i));
            message[8 + i] = (unsigned char)(clock >> (8 * i));
        }
        if (len > 0) {
            memcpy(message + 16, payload, len);
        }
    } else {
        Yoke__Plugin__V1__Data data = YOKE__PLUGIN__V1__DATA__INIT;
        data.sequence = sequence;
        data.payload.data = (uint8_t *)payload;
        data.payload.len = len;
        Yoke__Plugin__V1__Envelope e = YOKE__PLUGIN__V1__ENVELOPE__INIT;
        e.payload_case = YOKE__PLUGIN__V1__ENVELOPE__PAYLOAD_DATA;
        e.data = &data;
        char id[YOKE_ID_SIZE];
        yoke_seal(&u->envelopes, &e, id);
        size = protobuf_c_message_get_packed_size(&e.base);
        message = malloc(size + 1);
        protobuf_c_message_pack(&e.base, message);
    }
    if (send(f->fd, message, size, MSG_NOSIGNAL) < 0) {
        yoke_error_refuse(err, "stream.inactive", "the transport of %s took nothing: %s", stream,
                          strerror(errno));
        result = -1;
    }
    pthread_mutex_unlock(&f->mu);
    free(message);
    release(u, f);
    return result;
}

/* ---- the end ---- */

/* Ends the Session once: the end is handed over and nothing follows it. */
static void finish(yoke_unit *u, bool closed, char *cause, char *line) {
    pthread_mutex_lock(&u->mu);
    if (u->ended) {
        pthread_mutex_unlock(&u->mu);
        free(cause);
        free(line);
        return;
    }
    u->ended = true;
    flow *active = u->active;
    u->active = NULL;
    while (u->writes != NULL) {
        pending *w = u->writes;
        u->writes = w->next;
        if (w->message != NULL) {
            grpc_byte_buffer_destroy(w->message);
        }
        free(w);
    }
    u->last = NULL;
    yoke_event end = {.kind = YOKE_ENDED, .closed = closed, .cause = cause, .line = line};
    push(u, &end);
    pthread_mutex_unlock(&u->mu);

    while (active != NULL) {
        flow *f = active;
        active = f->link;
        shutdown(f->fd, SHUT_RDWR);
        release(u, f);
    }
    grpc_call_cancel(u->call, NULL);
    if (u->listener >= 0) {
        close(u->listener);
        u->listener = -1;
        unlink(u->bind);
    }
}

static void lost(yoke_unit *u, const char *why) {
    pthread_mutex_lock(&u->mu);
    bool closing = u->closing;
    pthread_mutex_unlock(&u->mu);
    if (closing) {
        finish(u, true, NULL, NULL);
        return;
    }
    char *line = NULL;
    if (asprintf(&line, "the Session's stream ended: %s", why) < 0) {
        line = NULL;
    }
    finish(u, false, strdup("liveness lost"), line);
}

/* ---- reading ---- */

static void read_next(yoke_unit *u, bool first) {
    grpc_op ops[2] = {0};
    size_t n = 0;
    if (first) {
        ops[n].op = GRPC_OP_RECV_INITIAL_METADATA;
        ops[n].data.recv_initial_metadata.recv_initial_metadata = &u->initial;
        n++;
    }
    ops[n].op = GRPC_OP_RECV_MESSAGE;
    ops[n].data.recv_message.recv_message = &u->incoming;
    n++;
    u->outstanding++;
    grpc_call_start_batch(u->call, ops, n, &u->tags[READ], NULL);
}

static void copy_bytes(ProtobufCBinaryData from, unsigned char **to, size_t *len) {
    *len = from.len;
    *to = malloc(from.len + 1);
    if (from.len > 0) {
        memcpy(*to, from.data, from.len);
    }
}

/* What one envelope from the Core means. */
static void handle(yoke_unit *u, const Yoke__Plugin__V1__Envelope *e) {
    switch (e->payload_case) {
    case YOKE__PLUGIN__V1__ENVELOPE__PAYLOAD_SESSION:
        if (e->session->kind_case == YOKE__PLUGIN__V1__SESSION_MESSAGE__KIND_REVOKED) {
            const Yoke__Plugin__V1__SessionMessage__Revoked *r = e->session->revoked;
            finish(u, false,
                   spoken(&yoke__plugin__v1__session_message__revoked__cause__descriptor, (int)r->cause,
                          "CAUSE_"),
                   strdup(r->line ? r->line : ""));
        }
        return;
    case YOKE__PLUGIN__V1__ENVELOPE__PAYLOAD_CONTROL:
        switch (e->control->kind_case) {
        case YOKE__PLUGIN__V1__CONTROL__KIND_COMMAND: {
            yoke_event ev = {
                .kind = YOKE_COMMAND, .id = strdup(e->message_id), .type = strdup(e->control->command->type)};
            copy_bytes(e->control->command->payload, &ev.payload, &ev.payload_len);
            push_now(u, &ev);
            return;
        }
        case YOKE__PLUGIN__V1__CONTROL__KIND_ACTIVATE: {
            const Yoke__Plugin__V1__Control__Activate *a = e->control->activate;
            flow *f = NULL;
            char *why = NULL;
            if (connect_flow(a, &f, &why) != 0) {
                acknowledge(u, e->message_id, YOKE__PLUGIN__V1__ACK__OUTCOME__OUTCOME_FAILED,
                            why ? why : "the transport cannot be reached");
                free(why);
                return;
            }
            pthread_mutex_lock(&u->mu);
            flow *former = unlink_flow(u, a->stream);
            f->link = u->active;
            u->active = f;
            pthread_mutex_unlock(&u->mu);
            if (former != NULL) {
                release(u, former);
            }
            acknowledge(u, e->message_id, YOKE__PLUGIN__V1__ACK__OUTCOME__OUTCOME_DONE, "");
            yoke_event ev = {.kind = YOKE_ACTIVATED,
                             .stream = strdup(a->stream),
                             .transport = strdup(transport_name(a->transport)),
                             .address = strdup(a->address)};
            push_now(u, &ev);
            return;
        }
        case YOKE__PLUGIN__V1__CONTROL__KIND_STOP: {
            const char *stream = e->control->stop->stream;
            pthread_mutex_lock(&u->mu);
            flow *f = unlink_flow(u, stream);
            pthread_mutex_unlock(&u->mu);
            if (f != NULL) {
                release(u, f);
            }
            acknowledge(u, e->message_id, YOKE__PLUGIN__V1__ACK__OUTCOME__OUTCOME_DONE, "");
            yoke_event ev = {.kind = YOKE_STOPPED, .stream = strdup(stream)};
            push_now(u, &ev);
            return;
        }
        default:
            return;
        }
    case YOKE__PLUGIN__V1__ENVELOPE__PAYLOAD_QUERY:
        if (e->query->kind_case == YOKE__PLUGIN__V1__QUERY__KIND_QUESTION) {
            yoke_event ev = {
                .kind = YOKE_QUESTION, .id = strdup(e->message_id), .type = strdup(e->query->question->type)};
            copy_bytes(e->query->question->payload, &ev.payload, &ev.payload_len);
            push_now(u, &ev);
        }
        return;
    case YOKE__PLUGIN__V1__ENVELOPE__PAYLOAD_ERROR: {
        yoke_event ev = {.kind = YOKE_REFUSED, .correlation = strdup(e->correlation_id)};
        yoke_refusal_of(e->error, &ev.error);
        push_now(u, &ev);
        return;
    }
    default:
        return;
    }
}

static void *read_session(void *arg) {
    yoke_unit *u = arg;
    for (;;) {
        grpc_event ev = grpc_completion_queue_next(u->cq, gpr_inf_future(GPR_CLOCK_REALTIME), NULL);
        if (ev.type == GRPC_QUEUE_SHUTDOWN) {
            return NULL;
        }
        kind k = ((tag *)ev.tag)->kind;
        grpc_byte_buffer *arrived = NULL;
        pthread_mutex_lock(&u->mu);
        u->outstanding--;
        switch (k) {
        case SENT_METADATA:
            break;
        case WRITTEN:
            u->writing = false;
            if (u->in_flight != NULL) {
                grpc_byte_buffer_destroy(u->in_flight);
                u->in_flight = NULL;
            }
            start_write(u);
            break;
        case READ:
            arrived = ev.success ? u->incoming : NULL;
            u->incoming = NULL;
            break;
        case STATUS:
            u->status_in = true;
            break;
        case KINDS:
            break;
        }
        pthread_mutex_unlock(&u->mu);

        if (k == READ) {
            if (arrived == NULL) {
                lost(u, "the Core ended it");
            } else {
                size_t len = 0;
                unsigned char *bytes = yoke_bytes(arrived, &len);
                grpc_byte_buffer_destroy(arrived);
                Yoke__Plugin__V1__Envelope *e =
                    bytes ? yoke__plugin__v1__envelope__unpack(NULL, len, bytes) : NULL;
                free(bytes);
                if (e != NULL) {
                    handle(u, e);
                    yoke__plugin__v1__envelope__free_unpacked(e, NULL);
                }
                pthread_mutex_lock(&u->mu);
                if (!u->ended && !u->status_in) {
                    read_next(u, false);
                }
                pthread_mutex_unlock(&u->mu);
            }
        } else if (k == STATUS) {
            char *details = grpc_slice_to_c_string(u->details);
            lost(u, details[0] != '\0' ? details : "its status arrived");
            gpr_free(details);
        }

        pthread_mutex_lock(&u->mu);
        bool over = u->status_in && u->outstanding == 0;
        pthread_mutex_unlock(&u->mu);
        if (over) {
            grpc_completion_queue_shutdown(u->cq);
        }
    }
}

/* ---- beating ---- */

static long long monotonic_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (long long)t.tv_sec * 1000000000 + t.tv_nsec;
}

/*
 * Repeats the author's last health report at the interval the Core assigned, until the Session ends,
 * and ends a close the Core does not answer. Before the author's first report it sends nothing: a
 * grade is the author's statement, and a unit that never reports loses its liveness as a unit that
 * sends nothing does.
 */
static void *beat(void *arg) {
    yoke_unit *u = arg;
    long long next_beat = u->interval_ns > 0 ? monotonic_ns() + u->interval_ns : -1;
    pthread_mutex_lock(&u->mu);
    while (!u->ended) {
        long long wake = next_beat;
        if (u->closing && (wake < 0 || u->close_by < wake)) {
            wake = u->close_by;
        }
        if (wake < 0) {
            pthread_cond_wait(&u->changed, &u->mu);
            continue;
        }
        long long now = monotonic_ns();
        if (now < wake) {
            /* The condition's clock is the realtime one: wait for the difference. */
            long long rest = wake - now;
            struct timespec deadline;
            clock_gettime(CLOCK_REALTIME, &deadline);
            deadline.tv_sec += rest / 1000000000;
            deadline.tv_nsec += rest % 1000000000;
            if (deadline.tv_nsec >= 1000000000) {
                deadline.tv_sec++;
                deadline.tv_nsec -= 1000000000;
            }
            pthread_cond_timedwait(&u->changed, &u->mu, &deadline);
            continue;
        }
        if (u->closing && now >= u->close_by) {
            /* The Core ends the stream on a CLOSE; if it does not, the departure still is one. */
            pthread_mutex_unlock(&u->mu);
            finish(u, true, NULL, NULL);
            pthread_mutex_lock(&u->mu);
            continue;
        }
        if (next_beat >= 0 && now >= next_beat) {
            next_beat += u->interval_ns;
            pthread_mutex_unlock(&u->mu);
            pthread_mutex_lock(&u->reporting);
            if (u->reported) {
                Yoke__Plugin__V1__Health health = YOKE__PLUGIN__V1__HEALTH__INIT;
                health.grade = u->grade;
                health.line = u->line;
                Yoke__Plugin__V1__Envelope e = YOKE__PLUGIN__V1__ENVELOPE__INIT;
                e.payload_case = YOKE__PLUGIN__V1__ENVELOPE__PAYLOAD_HEALTH;
                e.health = &health;
                send_envelope(u, NULL, &e, NULL);
            }
            pthread_mutex_unlock(&u->reporting);
            pthread_mutex_lock(&u->mu);
        }
    }
    pthread_mutex_unlock(&u->mu);
    return NULL;
}

/* ---- starting ---- */

static const char *process_getenv(const char *name, void *context) {
    (void)context;
    return getenv(name);
}

yoke_unit *yoke_start(const yoke_declaration *declaration, yoke_error *err) {
    return yoke_start_with(declaration, process_getenv, NULL, err);
}

/* Binds the unit's own socket. */
static int bind_unit(const char *path, yoke_error *err) {
    struct sockaddr_un addr = {.sun_family = AF_UNIX};
    if (strlen(path) >= sizeof addr.sun_path) {
        yoke_error_say(err, "the unit's socket %s cannot be bound: the path is too long", path);
        return -1;
    }
    strcpy(addr.sun_path, path);
    unlink(path);
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0 || bind(fd, (struct sockaddr *)&addr, sizeof addr) != 0 || listen(fd, 16) != 0) {
        int code = errno;
        if (fd >= 0) {
            close(fd);
        }
        yoke_error_say(err, "the unit's socket %s cannot be bound: %s", path, strerror(code));
        return -1;
    }
    return fd;
}

/* Registers: one call, answered or failed; never tried again. */
static Yoke__Plugin__V1__RegisterResponse *register_unit(grpc_channel *channel, const yoke_env *env,
                                                         const yoke_declaration *d, yoke_error *err) {
    Yoke__Plugin__V1__Surface declared = YOKE__PLUGIN__V1__SURFACE__INIT;
    char **owned[2];
    claim(d, &declared, owned);
    Yoke__Plugin__V1__RegisterRequest request = YOKE__PLUGIN__V1__REGISTER_REQUEST__INIT;
    request.plugin = env->plugin;
    request.unit = env->unit;
    request.token = env->token;
    request.protocol = (uint32_t)yoke_plugin_contract();
    request.language = "c";
    request.sdk_line = YOKE_SDK_LINE;
    request.declared = &declared;
    grpc_byte_buffer *sent = yoke_pack(&request.base);
    free(owned[0]);
    free(owned[1]);

    grpc_completion_queue *cq = grpc_completion_queue_create_for_pluck(NULL);
    grpc_slice method = grpc_slice_from_static_string("/yoke.plugin.v1.Register/Register");
    grpc_call *call = grpc_channel_create_call(channel, NULL, GRPC_PROPAGATE_DEFAULTS, cq, method, NULL,
                                               gpr_inf_future(GPR_CLOCK_REALTIME), NULL);
    grpc_metadata_array initial, trailing;
    grpc_metadata_array_init(&initial);
    grpc_metadata_array_init(&trailing);
    grpc_byte_buffer *received = NULL;
    grpc_status_code status = GRPC_STATUS_UNKNOWN;
    grpc_slice details = grpc_empty_slice();
    grpc_op ops[6] = {0};
    ops[0].op = GRPC_OP_SEND_INITIAL_METADATA;
    ops[1].op = GRPC_OP_SEND_MESSAGE;
    ops[1].data.send_message.send_message = sent;
    ops[2].op = GRPC_OP_SEND_CLOSE_FROM_CLIENT;
    ops[3].op = GRPC_OP_RECV_INITIAL_METADATA;
    ops[3].data.recv_initial_metadata.recv_initial_metadata = &initial;
    ops[4].op = GRPC_OP_RECV_MESSAGE;
    ops[4].data.recv_message.recv_message = &received;
    ops[5].op = GRPC_OP_RECV_STATUS_ON_CLIENT;
    ops[5].data.recv_status_on_client.trailing_metadata = &trailing;
    ops[5].data.recv_status_on_client.status = &status;
    ops[5].data.recv_status_on_client.status_details = &details;
    static tag registered = {READ};
    grpc_call_start_batch(call, ops, 6, &registered, NULL);
    grpc_completion_queue_pluck(cq, &registered, gpr_inf_future(GPR_CLOCK_REALTIME), NULL);

    Yoke__Plugin__V1__RegisterResponse *response = NULL;
    if (status != GRPC_STATUS_OK || received == NULL) {
        char *said = grpc_slice_to_c_string(details);
        yoke_error_say(err, "the registration at %s failed: %s", env->socket, said);
        gpr_free(said);
    } else {
        size_t len = 0;
        unsigned char *bytes = yoke_bytes(received, &len);
        response = bytes ? yoke__plugin__v1__register_response__unpack(NULL, len, bytes) : NULL;
        free(bytes);
        if (response == NULL) {
            yoke_error_say(err, "the registration's answer cannot be read");
        }
    }
    if (received != NULL) {
        grpc_byte_buffer_destroy(received);
    }
    grpc_byte_buffer_destroy(sent);
    grpc_slice_unref(details);
    grpc_metadata_array_destroy(&initial);
    grpc_metadata_array_destroy(&trailing);
    grpc_call_unref(call);
    grpc_completion_queue_shutdown(cq);
    grpc_completion_queue_destroy(cq);
    return response;
}

yoke_unit *yoke_start_with(const yoke_declaration *declaration, yoke_getenv getenv, void *context,
                           yoke_error *err) {
    yoke_env env = {0};
    if (yoke_environment(&env, getenv, context, err) != 0) {
        return NULL;
    }
    // Bind before registering: registered and unreachable is the one order that is wrong.
    int listener = bind_unit(env.bind, err);
    if (listener < 0) {
        yoke_env_clear(&env);
        return NULL;
    }
    grpc_init();
    grpc_channel *channel = yoke_dial(env.socket);
    Yoke__Plugin__V1__RegisterResponse *response = register_unit(channel, &env, declaration, err);
    if (response == NULL ||
        response->outcome == YOKE__PLUGIN__V1__REGISTER_RESPONSE__OUTCOME__OUTCOME_REFUSED) {
        if (response != NULL) {
            char *stage = spoken(&yoke__plugin__v1__stage__descriptor, (int)response->stage, "STAGE_");
            yoke_error_refuse(err, response->code, "%s", response->message);
            if (err != NULL) {
                err->stage = stage;
            } else {
                free(stage);
            }
            yoke__plugin__v1__register_response__free_unpacked(response, NULL);
        }
        grpc_channel_destroy(channel);
        grpc_shutdown();
        close(listener);
        unlink(env.bind);
        yoke_env_clear(&env);
        return NULL;
    }

    yoke_unit *u = calloc(1, sizeof *u);
    for (int i = 0; i < KINDS; i++) {
        u->tags[i].kind = (kind)i;
    }
    u->admission.restricted =
        response->outcome == YOKE__PLUGIN__V1__REGISTER_RESPONSE__OUTCOME__OUTCOME_ACCEPTED_WITH_RESTRICTIONS;
    scope_of(response->granted, &u->admission.granted);
    scope_of(response->withheld, &u->admission.withheld);
    if (response->heartbeat != NULL && response->heartbeat->interval != NULL) {
        u->interval_ns =
            response->heartbeat->interval->seconds * 1000000000LL + response->heartbeat->interval->nanos;
    }
    yoke_envelopes_init(&u->envelopes, response->session_id);
    yoke__plugin__v1__register_response__free_unpacked(response, NULL);
    u->channel = channel;
    u->listener = listener;
    u->bind = strdup(env.bind);
    yoke_env_clear(&env);
    pthread_mutex_init(&u->mu, NULL);
    pthread_cond_init(&u->changed, NULL);
    pthread_mutex_init(&u->reporting, NULL);
    grpc_metadata_array_init(&u->initial);
    grpc_metadata_array_init(&u->trailing);
    u->details = grpc_empty_slice();

    u->cq = grpc_completion_queue_create_for_next(NULL);
    grpc_slice method = grpc_slice_from_static_string("/yoke.plugin.v1.Session/Open");
    u->call = grpc_channel_create_call(channel, NULL, GRPC_PROPAGATE_DEFAULTS, u->cq, method, NULL,
                                       gpr_inf_future(GPR_CLOCK_REALTIME), NULL);
    pthread_mutex_lock(&u->mu);
    grpc_op meta = {.op = GRPC_OP_SEND_INITIAL_METADATA};
    u->outstanding++;
    grpc_call_start_batch(u->call, &meta, 1, &u->tags[SENT_METADATA], NULL);
    grpc_op status = {.op = GRPC_OP_RECV_STATUS_ON_CLIENT};
    status.data.recv_status_on_client.trailing_metadata = &u->trailing;
    status.data.recv_status_on_client.status = &u->status;
    status.data.recv_status_on_client.status_details = &u->details;
    u->outstanding++;
    grpc_call_start_batch(u->call, &status, 1, &u->tags[STATUS], NULL);
    read_next(u, true);
    pthread_mutex_unlock(&u->mu);

    Yoke__Plugin__V1__SessionMessage__Open open = YOKE__PLUGIN__V1__SESSION_MESSAGE__OPEN__INIT;
    Yoke__Plugin__V1__SessionMessage session = YOKE__PLUGIN__V1__SESSION_MESSAGE__INIT;
    session.kind_case = YOKE__PLUGIN__V1__SESSION_MESSAGE__KIND_OPEN;
    session.open = &open;
    Yoke__Plugin__V1__Envelope e = YOKE__PLUGIN__V1__ENVELOPE__INIT;
    e.payload_case = YOKE__PLUGIN__V1__ENVELOPE__PAYLOAD_SESSION;
    e.session = &session;
    send_envelope(u, NULL, &e, NULL);

    pthread_create(&u->reader, NULL, read_session, u);
    pthread_create(&u->beater, NULL, beat, u);
    return u;
}

/* ---- what the author sends ---- */

int yoke_close(yoke_unit *u, yoke_error *err) {
    pthread_mutex_lock(&u->mu);
    if (u->ended) {
        pthread_mutex_unlock(&u->mu);
        return 0;
    }
    u->closing = true;
    u->close_by = monotonic_ns() + CLOSE_GRACE_NS;
    pthread_mutex_unlock(&u->mu);
    Yoke__Plugin__V1__SessionMessage__Close close = YOKE__PLUGIN__V1__SESSION_MESSAGE__CLOSE__INIT;
    Yoke__Plugin__V1__SessionMessage session = YOKE__PLUGIN__V1__SESSION_MESSAGE__INIT;
    session.kind_case = YOKE__PLUGIN__V1__SESSION_MESSAGE__KIND_CLOSE;
    session.close = &close;
    Yoke__Plugin__V1__Envelope e = YOKE__PLUGIN__V1__ENVELOPE__INIT;
    e.payload_case = YOKE__PLUGIN__V1__ENVELOPE__PAYLOAD_SESSION;
    e.session = &session;
    int result = send_envelope(u, NULL, &e, err);
    pthread_mutex_lock(&u->mu);
    if (!u->ended) {
        queue_write(u, NULL);
    }
    pthread_cond_broadcast(&u->changed);
    pthread_mutex_unlock(&u->mu);
    return result;
}

int yoke_ack(yoke_unit *u, const yoke_event *command, yoke_outcome outcome, const char *line,
             yoke_error *err) {
    Yoke__Plugin__V1__Ack ack = YOKE__PLUGIN__V1__ACK__INIT;
    ack.outcome = (Yoke__Plugin__V1__Ack__Outcome)outcome;
    ack.line = (char *)(line ? line : "");
    Yoke__Plugin__V1__Envelope e = YOKE__PLUGIN__V1__ENVELOPE__INIT;
    e.payload_case = YOKE__PLUGIN__V1__ENVELOPE__PAYLOAD_ACK;
    e.ack = &ack;
    return send_envelope(u, command->id, &e, err);
}

int yoke_answer(yoke_unit *u, const yoke_event *question, const void *payload, size_t len, yoke_error *err) {
    Yoke__Plugin__V1__Query__Answer answer = YOKE__PLUGIN__V1__QUERY__ANSWER__INIT;
    answer.payload.data = (uint8_t *)payload;
    answer.payload.len = len;
    Yoke__Plugin__V1__Query query = YOKE__PLUGIN__V1__QUERY__INIT;
    query.kind_case = YOKE__PLUGIN__V1__QUERY__KIND_ANSWER;
    query.answer = &answer;
    Yoke__Plugin__V1__Envelope e = YOKE__PLUGIN__V1__ENVELOPE__INIT;
    e.payload_case = YOKE__PLUGIN__V1__ENVELOPE__PAYLOAD_QUERY;
    e.query = &query;
    return send_envelope(u, question->id, &e, err);
}

int yoke_fail(yoke_unit *u, const char *about, const char *code, const char *message, yoke_error *err) {
    Yoke__Plugin__V1__Error error = YOKE__PLUGIN__V1__ERROR__INIT;
    error.code = (char *)code;
    error.message = (char *)(message ? message : "");
    Yoke__Plugin__V1__Envelope e = YOKE__PLUGIN__V1__ENVELOPE__INIT;
    e.payload_case = YOKE__PLUGIN__V1__ENVELOPE__PAYLOAD_ERROR;
    e.error = &error;
    return send_envelope(u, about, &e, err);
}

int yoke_report(yoke_unit *u, const char *occurrence, yoke_severity severity, const char *line,
                const void *detail, size_t len, yoke_error *err) {
    if (!severity.set) {
        yoke_error_say(err, "an occurrence is reported with the author's severity, and none was stated");
        return -1;
    }
    if (severity.value > 99) {
        yoke_error_say(err, "a severity runs from 0 to 99, and %u is not one", severity.value);
        return -1;
    }
    Yoke__Plugin__V1__Event event = YOKE__PLUGIN__V1__EVENT__INIT;
    event.occurrence = (char *)occurrence;
    event.severity = severity.value;
    event.line = (char *)(line ? line : "");
    event.detail.data = (uint8_t *)detail;
    event.detail.len = len;
    Yoke__Plugin__V1__Envelope e = YOKE__PLUGIN__V1__ENVELOPE__INIT;
    e.payload_case = YOKE__PLUGIN__V1__ENVELOPE__PAYLOAD_EVENT;
    e.event = &event;
    return send_envelope(u, NULL, &e, err);
}

int yoke_health(yoke_unit *u, unsigned grade, const char *line, yoke_error *err) {
    if (grade > 99) {
        yoke_error_say(err, "a grade runs from 0 to 99, and %u is not one", grade);
        return -1;
    }
    pthread_mutex_lock(&u->reporting);
    free(u->line);
    u->line = strdup(line ? line : "");
    u->grade = grade;
    u->reported = true;
    Yoke__Plugin__V1__Health health = YOKE__PLUGIN__V1__HEALTH__INIT;
    health.grade = grade;
    health.line = u->line;
    Yoke__Plugin__V1__Envelope e = YOKE__PLUGIN__V1__ENVELOPE__INIT;
    e.payload_case = YOKE__PLUGIN__V1__ENVELOPE__PAYLOAD_HEALTH;
    e.health = &health;
    int result = send_envelope(u, NULL, &e, err);
    pthread_mutex_unlock(&u->reporting);
    return result;
}

/* ---- releasing ---- */

void yoke_free(yoke_unit *u) {
    if (u == NULL) {
        return;
    }
    finish(u, true, NULL, NULL);
    pthread_join(u->reader, NULL);
    pthread_join(u->beater, NULL);
    grpc_call_unref(u->call);
    grpc_completion_queue_destroy(u->cq);
    grpc_channel_destroy(u->channel);
    grpc_metadata_array_destroy(&u->initial);
    grpc_metadata_array_destroy(&u->trailing);
    grpc_slice_unref(u->details);
    while (u->head != NULL) {
        queued *q = u->head;
        u->head = q->next;
        yoke_event_clear(&q->event);
        free(q);
    }
    scope_clear(&u->admission.granted);
    scope_clear(&u->admission.withheld);
    yoke_envelopes_destroy(&u->envelopes);
    pthread_mutex_destroy(&u->mu);
    pthread_cond_destroy(&u->changed);
    pthread_mutex_destroy(&u->reporting);
    free(u->line);
    free(u->bind);
    free(u);
    grpc_shutdown();
}
