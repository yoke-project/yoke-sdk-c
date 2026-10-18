#define _GNU_SOURCE
#include "core.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <grpc/byte_buffer_reader.h>
#include <grpc/grpc.h>
#include <grpc/grpc_security.h>
#include <grpc/support/alloc.h>

typedef enum { NEW_CALL, READ, WRITTEN, OVER, METADATA, SHUTDOWN, KINDS } what;

typedef struct tag {
    what what;
    core_conn *conn;
} tag;

typedef struct write {
    grpc_byte_buffer *message; /* NULL: the status */
    char *code, *message_text; /* the status's refusal, where it carries one */
    bool refusal;
    struct write *next;
} write;

typedef struct inbox {
    Yoke__Interface__V1__ClientFrame *frame;
    struct inbox *next;
} inbox;

struct core_conn {
    core *core;
    int number;
    grpc_call *call;
    tag tags[KINDS];
    int pending;
    bool over, closed, finished, scripted;
    int cancelled;
    grpc_byte_buffer *incoming;
    write *writes, *last;
    bool writing;
    grpc_byte_buffer *in_flight;
    grpc_metadata trailing[1];
    grpc_slice status_text;
    char *details; /* the status's details bin, kept until the status is sent */
    inbox *head, *tail;
    Yoke__Interface__V1__ClientFrame *held;
    pthread_t thread;
    struct core_conn *link;
};

typedef struct kept {
    Yoke__Interface__V1__ClientFrame *frame;
    int attachment;
} kept;

struct core {
    char socket[256];
    core_script script;
    void *arg;
    grpc_server *server;
    grpc_completion_queue *cq;
    pthread_t thread;
    pthread_mutex_t mu;
    pthread_cond_t changed;
    grpc_call *offered;
    grpc_call_details details;
    grpc_metadata_array offered_metadata;
    tag tags[KINDS];
    int attachments;
    core_conn *conns;
    kept *frames;
    size_t count, cap;
    const char **ends;
    size_t n_ends;
    bool stopping;
};

static unsigned char *bytes_of(grpc_byte_buffer *buffer, size_t *len) {
    grpc_byte_buffer_reader reader;
    grpc_byte_buffer_reader_init(&reader, buffer);
    grpc_slice all = grpc_byte_buffer_reader_readall(&reader);
    grpc_byte_buffer_reader_destroy(&reader);
    *len = GRPC_SLICE_LENGTH(all);
    unsigned char *out = malloc(*len + 1);
    memcpy(out, GRPC_SLICE_START_PTR(all), *len);
    grpc_slice_unref(all);
    return out;
}

static grpc_byte_buffer *pack(const ProtobufCMessage *m) {
    size_t len = protobuf_c_message_get_packed_size(m);
    unsigned char *buf = malloc(len + 1);
    protobuf_c_message_pack(m, buf);
    grpc_slice s = grpc_slice_from_copied_buffer((const char *)buf, len);
    free(buf);
    grpc_byte_buffer *out = grpc_raw_byte_buffer_create(&s, 1);
    grpc_slice_unref(s);
    return out;
}

static void offer(core *c) {
    grpc_call_details_init(&c->details);
    grpc_metadata_array_init(&c->offered_metadata);
    grpc_server_request_call(c->server, &c->offered, &c->details, &c->offered_metadata, c->cq, c->cq,
                             &c->tags[NEW_CALL]);
}

/* ---- the status a refusal is carried in: google.rpc.Status, whose details hold the refusal ---- */

static size_t put_varint(unsigned char *out, unsigned long long n) {
    size_t i = 0;
    do {
        unsigned char b = n & 0x7F;
        n >>= 7;
        out[i++] = n ? (b | 0x80) : b;
    } while (n);
    return i;
}

static size_t put_field(unsigned char *out, unsigned number, const void *body, size_t len) {
    size_t at = put_varint(out, (unsigned long long)number << 3 | 2);
    at += put_varint(out + at, len);
    memcpy(out + at, body, len);
    return at + len;
}

static char *refusal_status(const char *code, const char *message, size_t *len) {
    Yoke__Interface__V1__Refusal refusal = YOKE__INTERFACE__V1__REFUSAL__INIT;
    refusal.code = (char *)code;
    refusal.message = (char *)message;
    size_t rl = protobuf_c_message_get_packed_size(&refusal.base);
    unsigned char *r = malloc(rl + 1);
    protobuf_c_message_pack(&refusal.base, r);
    static const char url[] = "type.googleapis.com/yoke.interface.v1.Refusal";
    unsigned char any[512], status[1024];
    size_t al = put_field(any, 1, url, sizeof url - 1);
    al += put_field(any + al, 2, r, rl);
    free(r);
    size_t sl = put_varint(status, 1ull << 3);
    sl += put_varint(status + sl, 9); /* FAILED_PRECONDITION */
    sl += put_field(status + sl, 2, message, strlen(message));
    sl += put_field(status + sl, 3, any, al);
    char *out = malloc(sl);
    memcpy(out, status, sl);
    *len = sl;
    return out;
}

/* ---- writing ---- */

/* Starts the next write, if there is one and none is outstanding. Holds mu. */
static void start_write(core_conn *cn) {
    if (cn->writing || cn->writes == NULL || cn->over) {
        return;
    }
    write *w = cn->writes;
    cn->writes = w->next;
    if (cn->writes == NULL) {
        cn->last = NULL;
    }
    grpc_op op = {0};
    if (w->message != NULL) {
        op.op = GRPC_OP_SEND_MESSAGE;
        op.data.send_message.send_message = w->message;
    } else {
        op.op = GRPC_OP_SEND_STATUS_FROM_SERVER;
        op.data.send_status_from_server.status =
            w->refusal ? GRPC_STATUS_FAILED_PRECONDITION : GRPC_STATUS_OK;
        if (w->refusal) {
            size_t len;
            cn->details = refusal_status(w->code, w->message_text, &len);
            cn->trailing[0].key = grpc_slice_from_static_string("grpc-status-details-bin");
            cn->trailing[0].value = grpc_slice_from_copied_buffer(cn->details, len);
            op.data.send_status_from_server.trailing_metadata_count = 1;
            op.data.send_status_from_server.trailing_metadata = cn->trailing;
            cn->status_text = grpc_slice_from_copied_string(w->message_text);
            op.data.send_status_from_server.status_details = &cn->status_text;
        }
        cn->finished = true;
    }
    cn->writing = true;
    cn->in_flight = w->message;
    cn->pending++;
    grpc_call_start_batch(cn->call, &op, 1, &cn->tags[WRITTEN], NULL);
    free(w->code);
    free(w->message_text);
    free(w);
}

static void enqueue(core_conn *cn, grpc_byte_buffer *message, bool refusal, const char *code,
                    const char *text) {
    write *w = calloc(1, sizeof *w);
    w->message = message;
    w->refusal = refusal;
    w->code = code ? strdup(code) : NULL;
    w->message_text = text ? strdup(text) : NULL;
    pthread_mutex_lock(&cn->core->mu);
    if (cn->over) {
        if (message != NULL) {
            grpc_byte_buffer_destroy(message);
        }
        free(w->code);
        free(w->message_text);
        free(w);
    } else {
        if (cn->last != NULL) {
            cn->last->next = w;
        } else {
            cn->writes = w;
        }
        cn->last = w;
        start_write(cn);
    }
    pthread_mutex_unlock(&cn->core->mu);
}

static void read_frame(core_conn *cn) {
    grpc_op op = {.op = GRPC_OP_RECV_MESSAGE};
    op.data.recv_message.recv_message = &cn->incoming;
    cn->pending++;
    grpc_call_start_batch(cn->call, &op, 1, &cn->tags[READ], NULL);
}

/* Lets a call go once nothing is outstanding on it. Holds mu. */
static void release(core_conn *cn) {
    if (cn->call != NULL && cn->over && cn->pending == 0) {
        grpc_call_unref(cn->call);
        cn->call = NULL;
        pthread_cond_broadcast(&cn->core->changed);
    }
}

/* ---- the script's side ---- */

static void *run_script(void *arg) {
    core_conn *cn = arg;
    cn->core->script(cn, cn->core->arg);
    return NULL;
}

int core_conn_number(const core_conn *c) { return c->number; }

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

int core_next(core_conn *cn, const Yoke__Interface__V1__ClientFrame **frame, int timeout_ms) {
    core *c = cn->core;
    struct timespec by = deadline_in(timeout_ms < 0 ? 0 : timeout_ms);
    pthread_mutex_lock(&c->mu);
    if (cn->held != NULL) {
        yoke__interface__v1__client_frame__free_unpacked(cn->held, NULL);
        cn->held = NULL;
    }
    for (;;) {
        if (cn->head != NULL) {
            inbox *i = cn->head;
            cn->head = i->next;
            if (cn->head == NULL) {
                cn->tail = NULL;
            }
            cn->held = i->frame;
            free(i);
            *frame = cn->held;
            pthread_mutex_unlock(&c->mu);
            return CORE_FRAME;
        }
        if (cn->closed) {
            pthread_mutex_unlock(&c->mu);
            return CORE_CLOSED;
        }
        if (cn->over || c->stopping) {
            pthread_mutex_unlock(&c->mu);
            return CORE_GONE;
        }
        if (timeout_ms >= 0) {
            if (pthread_cond_timedwait(&c->changed, &c->mu, &by) == ETIMEDOUT && cn->head == NULL) {
                pthread_mutex_unlock(&c->mu);
                return CORE_TIMEOUT;
            }
        } else {
            pthread_cond_wait(&c->changed, &c->mu);
        }
    }
}

void core_send(core_conn *cn, const Yoke__Interface__V1__CoreFrame *frame) {
    enqueue(cn, pack(&frame->base), false, NULL, NULL);
}

void core_finish(core_conn *cn) { enqueue(cn, NULL, false, NULL, NULL); }

void core_refuse(core_conn *cn, const char *code, const char *message) {
    enqueue(cn, NULL, true, code, message);
}

static __thread Yoke__Interface__V1__CoreFrame say_frame;
static __thread Yoke__Interface__V1__Opening say_opening;
static __thread Yoke__Interface__V1__Snapshot say_snapshot;

const Yoke__Interface__V1__CoreFrame *core_opening(const char *subscription, unsigned version,
                                                   Yoke__Interface__V1__Record **records, size_t n) {
    yoke__interface__v1__snapshot__init(&say_snapshot);
    say_snapshot.at = 4;
    say_snapshot.records = records;
    say_snapshot.n_records = n;
    yoke__interface__v1__opening__init(&say_opening);
    say_opening.picture = &say_snapshot;
    say_opening.subscription = (char *)subscription;
    say_opening.version = version;
    yoke__interface__v1__core_frame__init(&say_frame);
    say_frame.carries_case = YOKE__INTERFACE__V1__CORE_FRAME__CARRIES_OPENING;
    say_frame.opening = &say_opening;
    return &say_frame;
}

const Yoke__Interface__V1__CoreFrame *core_refusal_frame(const char *call,
                                                         const Yoke__Interface__V1__Refusal *r) {
    yoke__interface__v1__core_frame__init(&say_frame);
    say_frame.call = (char *)call;
    say_frame.carries_case = YOKE__INTERFACE__V1__CORE_FRAME__CARRIES_REFUSAL;
    say_frame.refusal = (Yoke__Interface__V1__Refusal *)r;
    return &say_frame;
}

/* ---- the event loop ---- */

static void keep(core *c, int attachment, Yoke__Interface__V1__ClientFrame *frame) {
    if (c->count == c->cap) {
        c->cap = c->cap ? c->cap * 2 : 64;
        c->frames = realloc(c->frames, c->cap * sizeof *c->frames);
    }
    c->frames[c->count].frame = frame;
    c->frames[c->count].attachment = attachment;
    c->count++;
}

static Yoke__Interface__V1__ClientFrame *copy_frame(const Yoke__Interface__V1__ClientFrame *f) {
    size_t len = protobuf_c_message_get_packed_size(&f->base);
    unsigned char *bytes = malloc(len + 1);
    protobuf_c_message_pack(&f->base, bytes);
    Yoke__Interface__V1__ClientFrame *copy = yoke__interface__v1__client_frame__unpack(NULL, len, bytes);
    free(bytes);
    return copy;
}

static void note_end(core *c, const char *how) {
    c->ends = realloc(c->ends, (c->n_ends + 1) * sizeof *c->ends);
    c->ends[c->n_ends++] = how;
}

static void accept_call(core *c) {
    char *method = grpc_slice_to_c_string(c->details.method);
    grpc_call *call = c->offered;
    grpc_call_details_destroy(&c->details);
    grpc_metadata_array_destroy(&c->offered_metadata);
    if (strcmp(method, "/yoke.interface.v1.Interface/Attach") != 0) {
        grpc_call_cancel(call, NULL);
        grpc_call_unref(call);
        gpr_free(method);
        return;
    }
    gpr_free(method);
    core_conn *cn = calloc(1, sizeof *cn);
    cn->core = c;
    cn->call = call;
    for (int i = 0; i < KINDS; i++) {
        cn->tags[i].what = (what)i;
        cn->tags[i].conn = cn;
    }
    pthread_mutex_lock(&c->mu);
    cn->number = ++c->attachments;
    cn->link = c->conns;
    c->conns = cn;
    grpc_op meta = {.op = GRPC_OP_SEND_INITIAL_METADATA};
    cn->pending += 2;
    grpc_call_start_batch(call, &meta, 1, &cn->tags[METADATA], NULL);
    grpc_op close = {.op = GRPC_OP_RECV_CLOSE_ON_SERVER};
    close.data.recv_close_on_server.cancelled = &cn->cancelled;
    grpc_call_start_batch(call, &close, 1, &cn->tags[OVER], NULL);
    read_frame(cn);
    pthread_cond_broadcast(&c->changed);
    pthread_mutex_unlock(&c->mu);
    cn->scripted = true;
    pthread_create(&cn->thread, NULL, run_script, cn);
}

static void *serve(void *arg) {
    core *c = arg;
    for (;;) {
        grpc_event ev = grpc_completion_queue_next(c->cq, gpr_inf_future(GPR_CLOCK_REALTIME), NULL);
        if (ev.type == GRPC_QUEUE_SHUTDOWN) {
            return NULL;
        }
        tag *t = ev.tag;
        core_conn *cn = t->conn;
        switch (t->what) {
        case NEW_CALL:
            if (!ev.success) {
                break; /* the server is shutting down */
            }
            accept_call(c);
            offer(c);
            break;
        case METADATA:
            pthread_mutex_lock(&c->mu);
            cn->pending--;
            release(cn);
            pthread_mutex_unlock(&c->mu);
            break;
        case READ: {
            pthread_mutex_lock(&c->mu);
            cn->pending--;
            if (ev.success && cn->incoming != NULL) {
                size_t len;
                unsigned char *bytes = bytes_of(cn->incoming, &len);
                grpc_byte_buffer_destroy(cn->incoming);
                cn->incoming = NULL;
                Yoke__Interface__V1__ClientFrame *f =
                    yoke__interface__v1__client_frame__unpack(NULL, len, bytes);
                free(bytes);
                if (f != NULL) {
                    keep(c, cn->number, copy_frame(f));
                    inbox *i = calloc(1, sizeof *i);
                    i->frame = f;
                    if (cn->tail != NULL) {
                        cn->tail->next = i;
                    } else {
                        cn->head = i;
                    }
                    cn->tail = i;
                }
                if (!cn->over) {
                    read_frame(cn);
                }
            } else if (!cn->closed) {
                /* The client's requests ended: it closed, unless the connection went away. */
                cn->closed = true;
                note_end(c, cn->cancelled ? "went away" : "closed");
            }
            pthread_cond_broadcast(&c->changed);
            release(cn);
            pthread_mutex_unlock(&c->mu);
            break;
        }
        case WRITTEN:
            pthread_mutex_lock(&c->mu);
            cn->pending--;
            cn->writing = false;
            if (cn->in_flight != NULL) {
                grpc_byte_buffer_destroy(cn->in_flight);
                cn->in_flight = NULL;
            }
            start_write(cn);
            release(cn);
            pthread_mutex_unlock(&c->mu);
            break;
        case OVER:
            pthread_mutex_lock(&c->mu);
            cn->pending--;
            cn->over = true;
            if (!cn->closed) {
                cn->closed = true;
                note_end(c, "went away");
            }
            pthread_cond_broadcast(&c->changed);
            release(cn);
            pthread_mutex_unlock(&c->mu);
            break;
        case SHUTDOWN:
            grpc_completion_queue_shutdown(c->cq);
            break;
        case KINDS:
            break;
        }
    }
}

core *core_start(const char *dir, core_script script, void *arg) {
    grpc_init();
    core *c = calloc(1, sizeof *c);
    for (int i = 0; i < KINDS; i++) {
        c->tags[i].what = (what)i;
    }
    snprintf(c->socket, sizeof c->socket, "%s/panel.sock", dir);
    c->script = script;
    c->arg = arg;
    pthread_mutex_init(&c->mu, NULL);
    pthread_cond_init(&c->changed, NULL);
    c->cq = grpc_completion_queue_create_for_next(NULL);
    c->server = grpc_server_create(NULL, NULL);
    grpc_server_register_completion_queue(c->server, c->cq, NULL);
    char address[300];
    snprintf(address, sizeof address, "unix:%s", c->socket);
    grpc_server_credentials *credentials = grpc_insecure_server_credentials_create();
    if (!grpc_server_add_http2_port(c->server, address, credentials)) {
        fprintf(stderr, "core: cannot listen at %s\n", address);
        abort();
    }
    grpc_server_credentials_release(credentials);
    grpc_server_start(c->server);
    offer(c);
    pthread_create(&c->thread, NULL, serve, c);
    return c;
}

void core_stop(core *c) {
    pthread_mutex_lock(&c->mu);
    c->stopping = true;
    pthread_cond_broadcast(&c->changed);
    pthread_mutex_unlock(&c->mu);
    grpc_server_shutdown_and_notify(c->server, c->cq, &c->tags[SHUTDOWN]);
    grpc_server_cancel_all_calls(c->server);
    for (core_conn *cn = c->conns; cn != NULL; cn = cn->link) {
        if (cn->scripted) {
            pthread_join(cn->thread, NULL);
        }
    }
    pthread_join(c->thread, NULL);
    grpc_server_destroy(c->server);
    grpc_completion_queue_destroy(c->cq);
    while (c->conns != NULL) {
        core_conn *cn = c->conns;
        c->conns = cn->link;
        if (cn->call != NULL) {
            grpc_call_unref(cn->call);
        }
        while (cn->head != NULL) {
            inbox *i = cn->head;
            cn->head = i->next;
            yoke__interface__v1__client_frame__free_unpacked(i->frame, NULL);
            free(i);
        }
        if (cn->held != NULL) {
            yoke__interface__v1__client_frame__free_unpacked(cn->held, NULL);
        }
        while (cn->writes != NULL) {
            write *w = cn->writes;
            cn->writes = w->next;
            if (w->message != NULL) {
                grpc_byte_buffer_destroy(w->message);
            }
            free(w->code);
            free(w->message_text);
            free(w);
        }
        free(cn->details);
        free(cn);
    }
    for (size_t i = 0; i < c->count; i++) {
        yoke__interface__v1__client_frame__free_unpacked(c->frames[i].frame, NULL);
    }
    free(c->frames);
    free(c->ends);
    pthread_mutex_destroy(&c->mu);
    pthread_cond_destroy(&c->changed);
    free(c);
    grpc_shutdown();
}

const char *core_socket(const core *c) { return c->socket; }

int core_attachments(core *c) {
    pthread_mutex_lock(&c->mu);
    int n = c->attachments;
    pthread_mutex_unlock(&c->mu);
    return n;
}

size_t core_ends(core *c) {
    pthread_mutex_lock(&c->mu);
    size_t n = c->n_ends;
    pthread_mutex_unlock(&c->mu);
    return n;
}

const char *core_end(core *c, size_t i) {
    pthread_mutex_lock(&c->mu);
    const char *how = i < c->n_ends ? c->ends[i] : NULL;
    pthread_mutex_unlock(&c->mu);
    return how;
}

size_t core_frames(core *c) {
    pthread_mutex_lock(&c->mu);
    size_t n = c->count;
    pthread_mutex_unlock(&c->mu);
    return n;
}

const Yoke__Interface__V1__ClientFrame *core_frame(core *c, size_t i, int *attachment) {
    pthread_mutex_lock(&c->mu);
    const Yoke__Interface__V1__ClientFrame *f = NULL;
    if (i < c->count) {
        f = c->frames[i].frame;
        if (attachment != NULL) {
            *attachment = c->frames[i].attachment;
        }
    }
    pthread_mutex_unlock(&c->mu);
    return f;
}

bool core_await_frames(core *c, size_t count, int timeout_ms) {
    struct timespec by = deadline_in(timeout_ms);
    pthread_mutex_lock(&c->mu);
    while (c->count < count) {
        if (pthread_cond_timedwait(&c->changed, &c->mu, &by) != 0) {
            break;
        }
    }
    bool got = c->count >= count;
    pthread_mutex_unlock(&c->mu);
    return got;
}
