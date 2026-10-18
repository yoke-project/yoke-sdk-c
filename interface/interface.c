/*
 * The interface library. One thread reads the attachment's completion queue and hands every frame to the
 * call it belongs to; a call waits on its own queue of frames, so one nobody is reading never holds up
 * another. Writes wait in order and go one at a time, which is all gRPC allows on a call, so no function
 * that sends ever waits for the Core.
 */
#include "yoke/interface.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include <grpc/grpc_posix.h>
#include <grpc/grpc_security.h>
#include <grpc/support/alloc.h>

#include "tls.h"
#include "wire.h"
#include "yoke/interface/v1/contract.pb-c.h"

/* How long a close waits for the Core to end an attachment told it is closing. */
#define CLOSE_GRACE_MS 1000

/* How long the TLS handshake and the dialling that comes before it may take. */
#define HANDSHAKE_MS 10000

/* The call identity of the credential an attachment presents first. */
#define AUTHENTICATING "c-1"

/* The size of a frame's header on a per-subscriber socket, and of the largest one it carries. */
#define HEADER 16
#define PACKET (HEADER + (1 << 20))

typedef enum { SENT_METADATA, READ, WRITTEN, STATUS, KINDS } kind;

typedef struct tag {
    kind kind;
} tag;

/* One write the attachment waits to make: a message, or the end of this side of the stream. */
typedef struct pending {
    grpc_byte_buffer *message; /* NULL: the half-close */
    struct pending *next;
} pending;

/* One frame of a call, in the order it arrived. */
typedef struct node {
    Yoke__Interface__V1__CoreFrame *frame;
    struct node *next;
} node;

/* The frames of one call. */
typedef struct callq {
    char *id;
    node *head, *tail;
    struct callq *link;
} callq;

struct yoke_attachment {
    grpc_channel *channel;
    grpc_completion_queue *cq;
    grpc_call *call;
    yoke_pump *pump;
    tag tags[KINDS];
    pthread_t reader;
    int timeout_ms;

    pthread_mutex_t mu;
    pthread_cond_t changed;

    pending *writes, *last;
    bool writing;
    grpc_byte_buffer *in_flight; /* kept until its write completes, as gRPC requires */

    grpc_byte_buffer *incoming;
    grpc_metadata_array initial, trailing;
    grpc_status_code status;
    grpc_slice details;
    int outstanding; /* batches started on the call and not yet completed */
    bool status_in;

    Yoke__Interface__V1__CoreFrame *opening; /* the first frame, once it has arrived */
    bool unopened;                           /* what arrived first was not an opening */
    bool ended, closing, hung;
    yoke_error failure; /* what ended the stream before it opened, as the Core said it */
    unsigned long long count;
    callq *calls;
    yoke_subscription *standing;
    Yoke__Interface__V1__Authenticated *established;
};

struct yoke_subscription {
    yoke_attachment *a;
    callq *q;
    char *id;
    Yoke__Interface__V1__Subscribed *pending; /* the answer to subscribing, the first delivery */
    bool standing;
};

struct yoke_delivery {
    yoke_attachment *a;
    char *id;
    bool flowing;
    int fd;   /* a per-subscriber socket the Core listens on; -1 on the connection */
    callq *q; /* the connection's frames */
};

int yoke_interface_contract(void) { return YOKE__INTERFACE__V1__CONTRACT__CONTRACT_VERSION; }

/* ---- addresses ---- */

static bool names_nothing(const char *name) {
    return name == NULL || name[0] == '\0' || strcmp(name, ".") == 0 || strcmp(name, "..") == 0 ||
           strchr(name, '/') != NULL;
}

static int channel_socket(char **address, const char *root, const char *channel, yoke_error *err) {
    if (names_nothing(channel)) {
        yoke_error_say(err, "the channel name \"%s\" cannot name a channel", channel ? channel : "");
        return -1;
    }
    if (asprintf(address, "%s/interfaces/%s.sock", root, channel) < 0) {
        *address = NULL;
        yoke_error_say(err, "out of memory");
        return -1;
    }
    return 0;
}

int yoke_interface_service(char **address, const char *runtime_dir, const char *channel, yoke_error *err) {
    return channel_socket(address, runtime_dir, channel, err);
}

int yoke_interface_application(char **address, const char *name, const char *channel, yoke_getenv getenv,
                               void *context, yoke_error *err) {
    if (names_nothing(name)) {
        yoke_error_say(err, "the instance name \"%s\" cannot name an instance", name ? name : "");
        return -1;
    }
    const char *runtime = getenv("XDG_RUNTIME_DIR", context);
    if (runtime == NULL || runtime[0] == '\0') {
        yoke_error_say(err,
                       "XDG_RUNTIME_DIR is not set, and an application instance's addresses derive from it");
        return -1;
    }
    char *root = NULL;
    if (asprintf(&root, "%s/yoke/%s", runtime, name) < 0) {
        yoke_error_say(err, "out of memory");
        return -1;
    }
    int result = channel_socket(address, root, channel, err);
    free(root);
    return result;
}

int yoke_interface_managed(char **address, yoke_getenv getenv, void *context, yoke_error *err) {
    const char *socket = getenv("YOKE_SOCKET", context);
    if (socket == NULL || socket[0] == '\0') {
        yoke_error_say(err,
                       "YOKE_SOCKET is not set: a managed interface is handed its channel's address, and "
                       "this process was not");
        return -1;
    }
    *address = strdup(socket);
    return 0;
}

/* ---- errors ---- */

static void refusal_into(const Yoke__Interface__V1__Refusal *r, yoke_error *err) {
    if (err == NULL) {
        return;
    }
    yoke_error_refuse(err, r->code, "%s", r->message ? r->message : "");
    switch (r->detail_case) {
    case YOKE__INTERFACE__V1__REFUSAL__DETAIL_SUBJECT:
        err->subject.kind = yoke_strdup(r->subject->kind);
        err->subject.identity = yoke_strdup(r->subject->identity);
        err->subject.incarnation = r->subject->incarnation;
        break;
    case YOKE__INTERFACE__V1__REFUSAL__DETAIL_ITEM:
        err->item = yoke_strdup(r->item);
        break;
    case YOKE__INTERFACE__V1__REFUSAL__DETAIL_SUSPENSION:
        err->suspension.grade = yoke_strdup(r->suspension->grade);
        err->suspension.by = yoke_strdup(r->suspension->by);
        break;
    default:
        break;
    }
}

static void ended_into(yoke_error *err) { yoke_error_say(err, "the attachment ended"); }

/* The varint at b, and the length it took; 0 when there is none. */
static size_t varint(const unsigned char *b, size_t len, uint64_t *value) {
    *value = 0;
    for (size_t i = 0; i < len && i < 10; i++) {
        *value |= (uint64_t)(b[i] & 0x7F) << (7 * i);
        if (!(b[i] & 0x80)) {
            return i + 1;
        }
    }
    return 0;
}

/*
 * The refusal a status carries, in its trailing metadata as `google.rpc.Status` whose details are the
 * refusals the Core attaches; false where it carries none.
 */
static bool refusal_of_status(const grpc_metadata_array *trailing, yoke_error *err) {
    for (size_t i = 0; i < trailing->count; i++) {
        const grpc_metadata *m = &trailing->metadata[i];
        if (GRPC_SLICE_LENGTH(m->key) != strlen("grpc-status-details-bin") ||
            memcmp(GRPC_SLICE_START_PTR(m->key), "grpc-status-details-bin", GRPC_SLICE_LENGTH(m->key)) != 0) {
            continue;
        }
        const unsigned char *status = GRPC_SLICE_START_PTR(m->value);
        size_t len = GRPC_SLICE_LENGTH(m->value), at = 0;
        while (at < len) {
            uint64_t tagged, n;
            size_t took = varint(status + at, len - at, &tagged);
            if (took == 0) {
                break;
            }
            at += took;
            unsigned wire = tagged & 7, number = (unsigned)(tagged >> 3);
            if (wire == 0) {
                took = varint(status + at, len - at, &n);
                if (took == 0) {
                    break;
                }
                at += took;
            } else if (wire == 2) {
                took = varint(status + at, len - at, &n);
                if (took == 0 || at + took + n > len) {
                    break;
                }
                at += took;
                if (number == 3) {
                    /* an Any: its type url, then its value */
                    const unsigned char *any = status + at;
                    size_t alen = n, aat = 0;
                    const unsigned char *url = NULL, *value = NULL;
                    size_t urllen = 0, vallen = 0;
                    while (aat < alen) {
                        uint64_t t2, l2;
                        size_t k = varint(any + aat, alen - aat, &t2);
                        if (k == 0 || (t2 & 7) != 2) {
                            break;
                        }
                        aat += k;
                        k = varint(any + aat, alen - aat, &l2);
                        if (k == 0 || aat + k + l2 > alen) {
                            break;
                        }
                        aat += k;
                        if ((t2 >> 3) == 1) {
                            url = any + aat, urllen = l2;
                        } else if ((t2 >> 3) == 2) {
                            value = any + aat, vallen = l2;
                        }
                        aat += l2;
                    }
                    static const char suffix[] = "yoke.interface.v1.Refusal";
                    if (url != NULL && value != NULL && urllen >= sizeof suffix - 1 &&
                        memcmp(url + urllen - (sizeof suffix - 1), suffix, sizeof suffix - 1) == 0) {
                        Yoke__Interface__V1__Refusal *r =
                            yoke__interface__v1__refusal__unpack(NULL, vallen, value);
                        if (r != NULL) {
                            refusal_into(r, err);
                            yoke__interface__v1__refusal__free_unpacked(r, NULL);
                            return true;
                        }
                    }
                }
                at += n;
            } else if (wire == 1) {
                at += 8;
            } else if (wire == 5) {
                at += 4;
            } else {
                break;
            }
        }
    }
    return false;
}

/* ---- time ---- */

static void deadline_in(int ms, struct timespec *t) {
    clock_gettime(CLOCK_REALTIME, t);
    t->tv_sec += ms / 1000;
    t->tv_nsec += (long)(ms % 1000) * 1000000L;
    if (t->tv_nsec >= 1000000000L) {
        t->tv_sec++;
        t->tv_nsec -= 1000000000L;
    }
}

/* ---- writing ---- */

/* Starts the next write, if there is one and none is outstanding. Holds mu. */
static void start_write(yoke_attachment *a) {
    if (a->writing || a->writes == NULL || a->status_in) {
        return;
    }
    pending *w = a->writes;
    a->writes = w->next;
    if (a->writes == NULL) {
        a->last = NULL;
    }
    grpc_op op = {0};
    if (w->message != NULL) {
        op.op = GRPC_OP_SEND_MESSAGE;
        op.data.send_message.send_message = w->message;
    } else {
        op.op = GRPC_OP_SEND_CLOSE_FROM_CLIENT;
    }
    a->writing = true;
    a->in_flight = w->message;
    a->outstanding++;
    grpc_call_start_batch(a->call, &op, 1, &a->tags[WRITTEN], NULL);
    free(w);
}

/* Queues a write. Holds mu. */
static void queue_write(yoke_attachment *a, grpc_byte_buffer *message) {
    pending *w = calloc(1, sizeof *w);
    w->message = message;
    if (a->last != NULL) {
        a->last->next = w;
    } else {
        a->writes = w;
    }
    a->last = w;
    start_write(a);
}

/* Sends one frame of the client's. Holds mu. */
static void send_frame(yoke_attachment *a, Yoke__Interface__V1__ClientFrame *f) {
    queue_write(a, yoke_pack(&f->base));
}

/* ---- calls ---- */

static callq *find_call(yoke_attachment *a, const char *id) {
    for (callq *q = a->calls; q != NULL; q = q->link) {
        if (strcmp(q->id, id) == 0) {
            return q;
        }
    }
    return NULL;
}

/* The queue of a call, made if there is none. Holds mu. */
static callq *register_call(yoke_attachment *a, const char *id) {
    callq *q = find_call(a, id);
    if (q == NULL) {
        q = calloc(1, sizeof *q);
        q->id = strdup(id);
        q->link = a->calls;
        a->calls = q;
    }
    return q;
}

static void drain(callq *q) {
    while (q->head != NULL) {
        node *n = q->head;
        q->head = n->next;
        yoke__interface__v1__core_frame__free_unpacked(n->frame, NULL);
        free(n);
    }
    q->tail = NULL;
}

/* Lets a call's queue go. Holds mu. */
static void forget_call(yoke_attachment *a, callq *q) {
    for (callq **at = &a->calls; *at != NULL; at = &(*at)->link) {
        if (*at == q) {
            *at = q->link;
            break;
        }
    }
    drain(q);
    free(q->id);
    free(q);
}

/* Hands a frame to the call it belongs to; one for a call nobody registered is dropped. Holds mu. */
static void deliver(yoke_attachment *a, Yoke__Interface__V1__CoreFrame *f) {
    callq *q = find_call(a, f->call);
    if (q == NULL) {
        yoke__interface__v1__core_frame__free_unpacked(f, NULL);
        return;
    }
    node *n = calloc(1, sizeof *n);
    n->frame = f;
    if (q->tail != NULL) {
        q->tail->next = n;
    } else {
        q->head = n;
    }
    q->tail = n;
}

/*
 * The next frame of a call, waiting at most timeout_ms (-1: as long as it takes). NULL when the time ran
 * out, with *timed_out set, or when the attachment ended and nothing is left. Holds mu.
 */
static Yoke__Interface__V1__CoreFrame *pop(yoke_attachment *a, callq *q, int timeout_ms, bool *timed_out) {
    struct timespec by;
    if (timeout_ms >= 0) {
        deadline_in(timeout_ms, &by);
    }
    *timed_out = false;
    while (q->head == NULL) {
        if (a->ended) {
            return NULL;
        }
        if (timeout_ms >= 0) {
            if (pthread_cond_timedwait(&a->changed, &a->mu, &by) == ETIMEDOUT && q->head == NULL) {
                *timed_out = true;
                return NULL;
            }
        } else {
            pthread_cond_wait(&a->changed, &a->mu);
        }
    }
    node *n = q->head;
    q->head = n->next;
    if (q->head == NULL) {
        q->tail = NULL;
    }
    Yoke__Interface__V1__CoreFrame *f = n->frame;
    free(n);
    return f;
}

/* ---- reading ---- */

static void read_next(yoke_attachment *a, bool first) {
    grpc_op ops[2] = {0};
    size_t n = 0;
    if (first) {
        ops[n].op = GRPC_OP_RECV_INITIAL_METADATA;
        ops[n].data.recv_initial_metadata.recv_initial_metadata = &a->initial;
        n++;
    }
    ops[n].op = GRPC_OP_RECV_MESSAGE;
    ops[n].data.recv_message.recv_message = &a->incoming;
    n++;
    a->outstanding++;
    grpc_call_start_batch(a->call, ops, n, &a->tags[READ], NULL);
}

/* Ends the attachment once: nothing more arrives, and every waiter is told. Holds mu. */
static void finish(yoke_attachment *a) {
    if (a->ended) {
        return;
    }
    a->ended = true;
    while (a->writes != NULL) {
        pending *w = a->writes;
        a->writes = w->next;
        if (w->message != NULL) {
            grpc_byte_buffer_destroy(w->message);
        }
        free(w);
    }
    a->last = NULL;
    grpc_call_cancel(a->call, NULL);
    pthread_cond_broadcast(&a->changed);
}

/* What one frame from the Core means. Holds mu. */
static void handle(yoke_attachment *a, Yoke__Interface__V1__CoreFrame *f) {
    if (a->opening == NULL && !a->unopened) {
        if (f->carries_case == YOKE__INTERFACE__V1__CORE_FRAME__CARRIES_OPENING) {
            a->opening = f;
            /* The standing subscription's frames may follow at once: its queue is there before they are read.
             */
            if (f->opening->subscription != NULL && f->opening->subscription[0] != '\0') {
                register_call(a, f->opening->subscription);
            }
        } else {
            a->unopened = true;
            yoke__interface__v1__core_frame__free_unpacked(f, NULL);
        }
        pthread_cond_broadcast(&a->changed);
        return;
    }
    if (f->carries_case == YOKE__INTERFACE__V1__CORE_FRAME__CARRIES_ANSWER && f->answer != NULL &&
        f->answer->answer_case == YOKE__INTERFACE__V1__RESPONSE__ANSWER_STREAM_SUBSCRIBE &&
        f->answer->stream_subscribe->arrives_case == YOKE__INTERFACE__V1__DELIVERING__ARRIVES_CONNECTION) {
        /* A delivery on the connection is registered as its answer arrives, so none of its frames can
         * arrive before it is. */
        register_call(a, f->answer->stream_subscribe->delivery);
    }
    deliver(a, f);
    pthread_cond_broadcast(&a->changed);
}

static void *read_attachment(void *arg) {
    yoke_attachment *a = arg;
    for (;;) {
        grpc_event ev = grpc_completion_queue_next(a->cq, gpr_inf_future(GPR_CLOCK_REALTIME), NULL);
        if (ev.type == GRPC_QUEUE_SHUTDOWN) {
            return NULL;
        }
        kind k = ((tag *)ev.tag)->kind;
        pthread_mutex_lock(&a->mu);
        a->outstanding--;
        switch (k) {
        case SENT_METADATA:
            break;
        case WRITTEN:
            a->writing = false;
            if (a->in_flight != NULL) {
                grpc_byte_buffer_destroy(a->in_flight);
                a->in_flight = NULL;
            }
            start_write(a);
            break;
        case READ: {
            grpc_byte_buffer *arrived = ev.success ? a->incoming : NULL;
            a->incoming = NULL;
            if (arrived == NULL) {
                finish(a);
            } else {
                size_t len = 0;
                unsigned char *bytes = yoke_bytes(arrived, &len);
                grpc_byte_buffer_destroy(arrived);
                Yoke__Interface__V1__CoreFrame *f =
                    bytes ? yoke__interface__v1__core_frame__unpack(NULL, len, bytes) : NULL;
                free(bytes);
                if (f != NULL) {
                    handle(a, f);
                }
                if (!a->ended && !a->status_in) {
                    read_next(a, false);
                }
            }
            break;
        }
        case STATUS:
            a->status_in = true;
            if (a->opening == NULL) {
                /* The attachment ended before it opened: what the Core said is what it is refused with. */
                if (!refusal_of_status(&a->trailing, &a->failure)) {
                    char *details = grpc_slice_to_c_string(a->details);
                    yoke_error_say(&a->failure, "the attachment was not opened: %s",
                                   details[0] != '\0' ? details : "the Core ended it");
                    gpr_free(details);
                }
            }
            finish(a);
            break;
        case KINDS:
            break;
        }
        bool over = a->status_in && a->outstanding == 0;
        pthread_mutex_unlock(&a->mu);
        if (over) {
            grpc_completion_queue_shutdown(a->cq);
        }
    }
}

/* ---- attaching ---- */

static grpc_channel *dial_tcp(const char *address) {
    grpc_channel_credentials *credentials = grpc_insecure_credentials_create();
    grpc_channel *channel = grpc_channel_create(address, credentials, NULL);
    grpc_channel_credentials_release(credentials);
    return channel;
}

static void free_attachment(yoke_attachment *a) {
    while (a->calls != NULL) {
        forget_call(a, a->calls);
    }
    if (a->opening != NULL) {
        yoke__interface__v1__core_frame__free_unpacked(a->opening, NULL);
    }
    if (a->established != NULL) {
        yoke__interface__v1__authenticated__free_unpacked(a->established, NULL);
    }
    yoke_error_clear(&a->failure);
    pthread_mutex_destroy(&a->mu);
    pthread_cond_destroy(&a->changed);
    free(a);
}

/* Closes the stream in order and waits for the Core to end it; then lets everything go. */
static void let_go(yoke_attachment *a) {
    pthread_mutex_lock(&a->mu);
    if (a->hung) {
        pthread_mutex_unlock(&a->mu);
        return;
    }
    a->hung = true;
    if (!a->ended && !a->closing) {
        a->closing = true;
        queue_write(a, NULL);
    }
    struct timespec by;
    deadline_in(CLOSE_GRACE_MS, &by);
    while (!a->ended) {
        if (pthread_cond_timedwait(&a->changed, &a->mu, &by) == ETIMEDOUT) {
            break;
        }
    }
    finish(a);
    pthread_mutex_unlock(&a->mu);
    pthread_join(a->reader, NULL);
    grpc_completion_queue_destroy(a->cq);
    grpc_metadata_array_destroy(&a->initial);
    grpc_metadata_array_destroy(&a->trailing);
    grpc_slice_unref(a->details);
    grpc_call_unref(a->call);
    grpc_channel_destroy(a->channel);
    yoke_pump_free(a->pump);
    grpc_shutdown();
}

static void request_into(Yoke__Interface__V1__ClientFrame *frame, Yoke__Interface__V1__Request *r,
                         char *call) {
    frame->call = call;
    frame->carries_case = YOKE__INTERFACE__V1__CLIENT_FRAME__CARRIES_REQUEST;
    frame->request = r;
}

yoke_attachment *yoke_attach(const char *address, const yoke_attach_options *options, yoke_error *err) {
    yoke_attach_options o = {0};
    if (options != NULL) {
        o = *options;
    }
    yoke_pump *pump = NULL;
    grpc_init();
    grpc_channel *channel = NULL;
    if (address[0] == '/') {
        if (o.roots_pem != NULL) {
            yoke_error_say(err, "%s is a local socket, and a local socket is not reached over TLS", address);
            grpc_shutdown();
            return NULL;
        }
        channel = yoke_dial(address);
    } else if (o.roots_pem == NULL) {
        channel = dial_tcp(address);
    } else {
        if (o.roots_pem[0] == '\0') {
            yoke_error_say(err,
                           "TLS was asked with no roots, and the Core's chain is verified only against the "
                           "roots its caller supplies");
            grpc_shutdown();
            return NULL;
        }
        int fd = -1;
        pump = yoke_tls_connect(address, o.roots_pem, o.tls_name, HANDSHAKE_MS, &fd, err);
        if (pump == NULL) {
            grpc_shutdown();
            return NULL;
        }
        grpc_channel_credentials *credentials = grpc_insecure_credentials_create();
        channel = grpc_channel_create_from_fd("yoke-tls", fd, credentials, NULL);
        grpc_channel_credentials_release(credentials);
    }

    yoke_attachment *a = calloc(1, sizeof *a);
    for (int i = 0; i < KINDS; i++) {
        a->tags[i].kind = (kind)i;
    }
    a->timeout_ms = o.timeout_ms > 0 ? o.timeout_ms : -1;
    pthread_mutex_init(&a->mu, NULL);
    pthread_cond_init(&a->changed, NULL);
    grpc_metadata_array_init(&a->initial);
    grpc_metadata_array_init(&a->trailing);
    a->details = grpc_empty_slice();
    a->channel = channel;
    a->pump = pump;
    a->cq = grpc_completion_queue_create_for_next(NULL);
    grpc_slice method = grpc_slice_from_static_string("/yoke.interface.v1.Interface/Attach");
    a->call = grpc_channel_create_call(channel, NULL, GRPC_PROPAGATE_DEFAULTS, a->cq, method, NULL,
                                       gpr_inf_future(GPR_CLOCK_REALTIME), NULL);
    bool credential = o.account != NULL || o.token != NULL;
    pthread_mutex_lock(&a->mu);
    grpc_op meta = {.op = GRPC_OP_SEND_INITIAL_METADATA};
    a->outstanding++;
    grpc_call_start_batch(a->call, &meta, 1, &a->tags[SENT_METADATA], NULL);
    grpc_op status = {.op = GRPC_OP_RECV_STATUS_ON_CLIENT};
    status.data.recv_status_on_client.trailing_metadata = &a->trailing;
    status.data.recv_status_on_client.status = &a->status;
    status.data.recv_status_on_client.status_details = &a->details;
    a->outstanding++;
    grpc_call_start_batch(a->call, &status, 1, &a->tags[STATUS], NULL);
    read_next(a, true);
    if (credential) {
        /* The credential is the attachment's first frame, sent before the Core has said anything. */
        Yoke__Interface__V1__Authenticate auth = YOKE__INTERFACE__V1__AUTHENTICATE__INIT;
        Yoke__Interface__V1__Authenticate__Password password =
            YOKE__INTERFACE__V1__AUTHENTICATE__PASSWORD__INIT;
        if (o.account != NULL) {
            password.account = (char *)o.account;
            password.secret = (char *)(o.secret ? o.secret : "");
            auth.credential_case = YOKE__INTERFACE__V1__AUTHENTICATE__CREDENTIAL_PASSWORD;
            auth.password = &password;
        } else {
            auth.credential_case = YOKE__INTERFACE__V1__AUTHENTICATE__CREDENTIAL_TOKEN;
            auth.token = (char *)o.token;
        }
        Yoke__Interface__V1__Request request = YOKE__INTERFACE__V1__REQUEST__INIT;
        request.version = (uint32_t)yoke_interface_contract();
        request.operation_case = YOKE__INTERFACE__V1__REQUEST__OPERATION_AUTHENTICATE;
        request.authenticate = &auth;
        Yoke__Interface__V1__ClientFrame frame = YOKE__INTERFACE__V1__CLIENT_FRAME__INIT;
        request_into(&frame, &request, (char *)AUTHENTICATING);
        send_frame(a, &frame);
        a->count = 1;
        register_call(a, AUTHENTICATING);
    }
    pthread_mutex_unlock(&a->mu);
    pthread_create(&a->reader, NULL, read_attachment, a);

    /* Waits for the opening. */
    pthread_mutex_lock(&a->mu);
    struct timespec by;
    int wait_ms = a->timeout_ms;
    if (wait_ms >= 0) {
        deadline_in(wait_ms, &by);
    }
    bool timed_out = false;
    while (a->opening == NULL && !a->unopened && !a->ended && !timed_out) {
        if (wait_ms >= 0) {
            timed_out = pthread_cond_timedwait(&a->changed, &a->mu, &by) == ETIMEDOUT && a->opening == NULL;
        } else {
            pthread_cond_wait(&a->changed, &a->mu);
        }
    }
    bool opened = a->opening != NULL;
    pthread_mutex_unlock(&a->mu);
    if (!opened) {
        pthread_mutex_lock(&a->mu);
        if (a->unopened) {
            yoke_error_say(err, "the attachment opened with something that is not an opening");
        } else if (timed_out) {
            yoke_error_say(err, "the channel did not open within %d ms", wait_ms);
        } else if (a->failure.message != NULL || a->failure.code != NULL) {
            if (err != NULL) {
                yoke_error_clear(err);
                *err = a->failure;
                memset(&a->failure, 0, sizeof a->failure);
            }
        } else {
            ended_into(err);
        }
        pthread_mutex_unlock(&a->mu);
        let_go(a);
        free_attachment(a);
        return NULL;
    }
    Yoke__Interface__V1__Opening *opening = a->opening->opening;
    if (opening->version != (uint32_t)yoke_interface_contract()) {
        yoke_error_refuse(err, "compat.unsupported",
                          "this library speaks the interface contract's version %d, and the Core %u",
                          yoke_interface_contract(), opening->version);
        let_go(a);
        free_attachment(a);
        return NULL;
    }
    if (opening->subscription != NULL && opening->subscription[0] != '\0') {
        pthread_mutex_lock(&a->mu);
        yoke_subscription *s = calloc(1, sizeof *s);
        s->a = a;
        s->id = strdup(opening->subscription);
        s->q = find_call(a, s->id);
        s->standing = true;
        a->standing = s;
        pthread_mutex_unlock(&a->mu);
    }
    if (credential) {
        pthread_mutex_lock(&a->mu);
        callq *q = find_call(a, AUTHENTICATING);
        bool late;
        Yoke__Interface__V1__CoreFrame *f = pop(a, q, a->timeout_ms, &late);
        forget_call(a, q);
        pthread_mutex_unlock(&a->mu);
        if (f == NULL) {
            if (late) {
                yoke_error_say(err, "the Core did not answer the credential within %d ms", a->timeout_ms);
            } else {
                ended_into(err);
            }
        } else if (f->carries_case == YOKE__INTERFACE__V1__CORE_FRAME__CARRIES_REFUSAL) {
            refusal_into(f->refusal, err);
            yoke__interface__v1__core_frame__free_unpacked(f, NULL);
            f = NULL;
        } else if (f->carries_case == YOKE__INTERFACE__V1__CORE_FRAME__CARRIES_ANSWER &&
                   f->answer->answer_case == YOKE__INTERFACE__V1__RESPONSE__ANSWER_AUTHENTICATE) {
            size_t len = protobuf_c_message_get_packed_size(&f->answer->authenticate->base);
            unsigned char *bytes = malloc(len + 1);
            protobuf_c_message_pack(&f->answer->authenticate->base, bytes);
            a->established = yoke__interface__v1__authenticated__unpack(NULL, len, bytes);
            free(bytes);
            yoke__interface__v1__core_frame__free_unpacked(f, NULL);
            return a;
        } else {
            yoke_error_say(err, "the Core answered the credential with something that is not an answer");
        }
        if (f != NULL) {
            yoke__interface__v1__core_frame__free_unpacked(f, NULL);
        }
        if (a->standing != NULL) {
            free(a->standing->id);
            free(a->standing);
            a->standing = NULL;
        }
        let_go(a);
        free_attachment(a);
        return NULL;
    }
    return a;
}

void yoke_hangup(yoke_attachment *a) { let_go(a); }

void yoke_detach(yoke_attachment *a) {
    if (a == NULL) {
        return;
    }
    if (a->standing != NULL) {
        free(a->standing->id);
        free(a->standing);
        a->standing = NULL;
    }
    let_go(a);
    free_attachment(a);
}

const Yoke__Interface__V1__Snapshot *yoke_picture(const yoke_attachment *a) {
    return a->opening->opening->picture;
}

int yoke_interface_version(const yoke_attachment *a) { return (int)a->opening->opening->version; }

yoke_subscription *yoke_standing(yoke_attachment *a) { return a->standing; }

const Yoke__Interface__V1__Authenticated *yoke_established(const yoke_attachment *a) {
    return a->established;
}

/* ---- the operations ---- */

/* Sends a request under a new call identity, and registers the frames that answer it. Holds mu. */
static callq *issue(yoke_attachment *a, Yoke__Interface__V1__Request *r, yoke_error *err) {
    if (a->ended || a->closing) {
        ended_into(err);
        return NULL;
    }
    char id[32];
    snprintf(id, sizeof id, "c-%llu", ++a->count);
    callq *q = register_call(a, id);
    r->version = (uint32_t)yoke_interface_contract();
    Yoke__Interface__V1__ClientFrame frame = YOKE__INTERFACE__V1__CLIENT_FRAME__INIT;
    request_into(&frame, r, q->id);
    send_frame(a, &frame);
    return q;
}

static void cancel_call(yoke_attachment *a, callq *q) {
    Yoke__Interface__V1__Cancel cancel = YOKE__INTERFACE__V1__CANCEL__INIT;
    Yoke__Interface__V1__ClientFrame frame = YOKE__INTERFACE__V1__CLIENT_FRAME__INIT;
    frame.call = q->id;
    frame.carries_case = YOKE__INTERFACE__V1__CLIENT_FRAME__CARRIES_CANCEL;
    frame.cancel = &cancel;
    send_frame(a, &frame);
}

/*
 * A call's first frame as its answer: a refusal is the error, and an attachment that ended answers that it
 * ended. The Response is a copy, which the caller frees. Holds mu.
 */
static Yoke__Interface__V1__Response *first(yoke_attachment *a, callq *q, yoke_error *err) {
    bool late;
    Yoke__Interface__V1__CoreFrame *f = pop(a, q, a->timeout_ms, &late);
    if (f == NULL) {
        if (late) {
            yoke_error_say(err, "the Core did not answer within %d ms", a->timeout_ms);
            cancel_call(a, q);
        } else {
            ended_into(err);
        }
        return NULL;
    }
    Yoke__Interface__V1__Response *answer = NULL;
    if (f->carries_case == YOKE__INTERFACE__V1__CORE_FRAME__CARRIES_REFUSAL) {
        refusal_into(f->refusal, err);
    } else if (f->carries_case == YOKE__INTERFACE__V1__CORE_FRAME__CARRIES_ANSWER) {
        size_t len = protobuf_c_message_get_packed_size(&f->answer->base);
        unsigned char *bytes = malloc(len + 1);
        protobuf_c_message_pack(&f->answer->base, bytes);
        answer = yoke__interface__v1__response__unpack(NULL, len, bytes);
        free(bytes);
    } else {
        yoke_error_say(err, "the Core answered a call with something that is not an answer");
    }
    yoke__interface__v1__core_frame__free_unpacked(f, NULL);
    return answer;
}

/* One call: the request, and its answer, which must be the answer of the operation. */
static int call(yoke_attachment *a, Yoke__Interface__V1__Request *r,
                Yoke__Interface__V1__Response__AnswerCase want, Yoke__Interface__V1__Response **out,
                yoke_error *err) {
    pthread_mutex_lock(&a->mu);
    callq *q = issue(a, r, err);
    if (q == NULL) {
        pthread_mutex_unlock(&a->mu);
        return -1;
    }
    Yoke__Interface__V1__Response *answer = first(a, q, err);
    forget_call(a, q);
    pthread_mutex_unlock(&a->mu);
    if (answer == NULL) {
        return -1;
    }
    if (answer->answer_case != want) {
        yoke__interface__v1__response__free_unpacked(answer, NULL);
        yoke_error_say(err, "the Core answered with the answer of another operation");
        return -1;
    }
    if (out != NULL) {
        *out = answer;
    } else {
        yoke__interface__v1__response__free_unpacked(answer, NULL);
    }
    return 0;
}

void yoke_response_free(Yoke__Interface__V1__Response *answer) {
    if (answer != NULL) {
        yoke__interface__v1__response__free_unpacked(answer, NULL);
    }
}

static int authenticate_with(yoke_attachment *a, Yoke__Interface__V1__Authenticate *auth,
                             Yoke__Interface__V1__Response **answer, yoke_error *err) {
    Yoke__Interface__V1__Request r = YOKE__INTERFACE__V1__REQUEST__INIT;
    r.operation_case = YOKE__INTERFACE__V1__REQUEST__OPERATION_AUTHENTICATE;
    r.authenticate = auth;
    return call(a, &r, YOKE__INTERFACE__V1__RESPONSE__ANSWER_AUTHENTICATE, answer, err);
}

int yoke_authenticate(yoke_attachment *a, const char *account, const char *secret,
                      Yoke__Interface__V1__Response **answer, yoke_error *err) {
    Yoke__Interface__V1__Authenticate auth = YOKE__INTERFACE__V1__AUTHENTICATE__INIT;
    Yoke__Interface__V1__Authenticate__Password password = YOKE__INTERFACE__V1__AUTHENTICATE__PASSWORD__INIT;
    password.account = (char *)account;
    password.secret = (char *)secret;
    auth.credential_case = YOKE__INTERFACE__V1__AUTHENTICATE__CREDENTIAL_PASSWORD;
    auth.password = &password;
    return authenticate_with(a, &auth, answer, err);
}

int yoke_authenticate_token(yoke_attachment *a, const char *token, Yoke__Interface__V1__Response **answer,
                            yoke_error *err) {
    Yoke__Interface__V1__Authenticate auth = YOKE__INTERFACE__V1__AUTHENTICATE__INIT;
    auth.credential_case = YOKE__INTERFACE__V1__AUTHENTICATE__CREDENTIAL_TOKEN;
    auth.token = (char *)token;
    return authenticate_with(a, &auth, answer, err);
}

int yoke_read(yoke_attachment *a, const char *kind, const char *identity,
              Yoke__Interface__V1__Response **answer, yoke_error *err) {
    Yoke__Interface__V1__Read read = YOKE__INTERFACE__V1__READ__INIT;
    read.kind = (char *)kind;
    read.identity = (char *)(identity ? identity : "");
    Yoke__Interface__V1__Request r = YOKE__INTERFACE__V1__REQUEST__INIT;
    r.operation_case = YOKE__INTERFACE__V1__REQUEST__OPERATION_READ;
    r.read = &read;
    return call(a, &r, YOKE__INTERFACE__V1__RESPONSE__ANSWER_READ, answer, err);
}

int yoke_confirm(yoke_attachment *a, const char *subscription, uint64_t sequence,
                 Yoke__Interface__V1__Response **answer, yoke_error *err) {
    Yoke__Interface__V1__Confirm confirm = YOKE__INTERFACE__V1__CONFIRM__INIT;
    confirm.subscription = (char *)subscription;
    confirm.sequence = sequence;
    Yoke__Interface__V1__Request r = YOKE__INTERFACE__V1__REQUEST__INIT;
    r.operation_case = YOKE__INTERFACE__V1__REQUEST__OPERATION_CONFIRM;
    r.confirm = &confirm;
    return call(a, &r, YOKE__INTERFACE__V1__RESPONSE__ANSWER_CONFIRM, answer, err);
}

int yoke_command(yoke_attachment *a, const char *unit, const char *type, const void *payload, size_t len,
                 Yoke__Interface__V1__Response **answer, yoke_error *err) {
    Yoke__Interface__V1__Command command = YOKE__INTERFACE__V1__COMMAND__INIT;
    command.unit = (char *)unit;
    command.type = (char *)type;
    command.payload.data = (uint8_t *)payload;
    command.payload.len = len;
    Yoke__Interface__V1__Request r = YOKE__INTERFACE__V1__REQUEST__INIT;
    r.operation_case = YOKE__INTERFACE__V1__REQUEST__OPERATION_COMMAND;
    r.command = &command;
    return call(a, &r, YOKE__INTERFACE__V1__RESPONSE__ANSWER_COMMAND, answer, err);
}

int yoke_ask(yoke_attachment *a, const char *unit, const char *type, const void *payload, size_t len,
             Yoke__Interface__V1__Response **answer, yoke_error *err) {
    Yoke__Interface__V1__Question question = YOKE__INTERFACE__V1__QUESTION__INIT;
    question.unit = (char *)unit;
    question.type = (char *)type;
    question.payload.data = (uint8_t *)payload;
    question.payload.len = len;
    Yoke__Interface__V1__Request r = YOKE__INTERFACE__V1__REQUEST__INIT;
    r.operation_case = YOKE__INTERFACE__V1__REQUEST__OPERATION_QUERY;
    r.query = &question;
    return call(a, &r, YOKE__INTERFACE__V1__RESPONSE__ANSWER_QUERY, answer, err);
}

static int unit_stream(yoke_attachment *a, Yoke__Interface__V1__Request__OperationCase operation,
                       Yoke__Interface__V1__Response__AnswerCase want, const char *unit, const char *stream,
                       Yoke__Interface__V1__Response **answer, yoke_error *err) {
    Yoke__Interface__V1__UnitStream us = YOKE__INTERFACE__V1__UNIT_STREAM__INIT;
    us.unit = (char *)unit;
    us.stream = (char *)stream;
    Yoke__Interface__V1__Request r = YOKE__INTERFACE__V1__REQUEST__INIT;
    r.operation_case = operation;
    switch (operation) {
    case YOKE__INTERFACE__V1__REQUEST__OPERATION_STREAM_START:
        r.stream_start = &us;
        break;
    case YOKE__INTERFACE__V1__REQUEST__OPERATION_STREAM_STOP:
        r.stream_stop = &us;
        break;
    default:
        r.stream_subscribe = &us;
        break;
    }
    return call(a, &r, want, answer, err);
}

int yoke_start_stream(yoke_attachment *a, const char *unit, const char *stream,
                      Yoke__Interface__V1__Response **answer, yoke_error *err) {
    return unit_stream(a, YOKE__INTERFACE__V1__REQUEST__OPERATION_STREAM_START,
                       YOKE__INTERFACE__V1__RESPONSE__ANSWER_STREAM_START, unit, stream, answer, err);
}

int yoke_stop_stream(yoke_attachment *a, const char *unit, const char *stream,
                     Yoke__Interface__V1__Response **answer, yoke_error *err) {
    return unit_stream(a, YOKE__INTERFACE__V1__REQUEST__OPERATION_STREAM_STOP,
                       YOKE__INTERFACE__V1__RESPONSE__ANSWER_STREAM_STOP, unit, stream, answer, err);
}

int yoke_reclaim(yoke_attachment *a, Yoke__Interface__V1__Response **answer, yoke_error *err) {
    Yoke__Interface__V1__Reclaim reclaim = YOKE__INTERFACE__V1__RECLAIM__INIT;
    Yoke__Interface__V1__Request r = YOKE__INTERFACE__V1__REQUEST__INIT;
    r.operation_case = YOKE__INTERFACE__V1__REQUEST__OPERATION_RECLAIM;
    r.reclaim = &reclaim;
    return call(a, &r, YOKE__INTERFACE__V1__RESPONSE__ANSWER_RECLAIM, answer, err);
}

/* ---- subscriptions ---- */

yoke_subscription *yoke_subscribe(yoke_attachment *a, const Yoke__Interface__V1__Filter *filter,
                                  yoke_error *err) {
    Yoke__Interface__V1__Filter none = YOKE__INTERFACE__V1__FILTER__INIT;
    Yoke__Interface__V1__Subscribe subscribe = YOKE__INTERFACE__V1__SUBSCRIBE__INIT;
    subscribe.filter = (Yoke__Interface__V1__Filter *)(filter ? filter : &none);
    Yoke__Interface__V1__Request r = YOKE__INTERFACE__V1__REQUEST__INIT;
    r.operation_case = YOKE__INTERFACE__V1__REQUEST__OPERATION_SUBSCRIBE;
    r.subscribe = &subscribe;
    pthread_mutex_lock(&a->mu);
    callq *q = issue(a, &r, err);
    if (q == NULL) {
        pthread_mutex_unlock(&a->mu);
        return NULL;
    }
    Yoke__Interface__V1__Response *answer = first(a, q, err);
    if (answer == NULL || answer->answer_case != YOKE__INTERFACE__V1__RESPONSE__ANSWER_SUBSCRIBE) {
        if (answer != NULL) {
            yoke_error_say(err, "the Core answered with the answer of another operation");
            yoke__interface__v1__response__free_unpacked(answer, NULL);
        }
        forget_call(a, q);
        pthread_mutex_unlock(&a->mu);
        return NULL;
    }
    yoke_subscription *s = calloc(1, sizeof *s);
    s->a = a;
    s->q = q;
    s->id = strdup(q->id);
    /* The answer is the first delivery. */
    size_t len = protobuf_c_message_get_packed_size(&answer->subscribe->base);
    unsigned char *bytes = malloc(len + 1);
    protobuf_c_message_pack(&answer->subscribe->base, bytes);
    s->pending = yoke__interface__v1__subscribed__unpack(NULL, len, bytes);
    free(bytes);
    yoke__interface__v1__response__free_unpacked(answer, NULL);
    pthread_mutex_unlock(&a->mu);
    return s;
}

const char *yoke_subscription_id(const yoke_subscription *s) { return s->id; }

int yoke_subscription_next(yoke_subscription *s, Yoke__Interface__V1__Subscribed **out, int timeout_ms,
                           yoke_error *err) {
    if (s->pending != NULL) {
        *out = s->pending;
        s->pending = NULL;
        return YOKE_WAIT_GOT;
    }
    yoke_attachment *a = s->a;
    pthread_mutex_lock(&a->mu);
    for (;;) {
        bool late;
        Yoke__Interface__V1__CoreFrame *f = pop(a, s->q, timeout_ms, &late);
        if (f == NULL) {
            pthread_mutex_unlock(&a->mu);
            if (late) {
                return YOKE_WAIT_TIMEOUT;
            }
            ended_into(err);
            return YOKE_WAIT_FAILED;
        }
        int result = -2;
        Yoke__Interface__V1__Subscribed *delivery = NULL;
        switch (f->carries_case) {
        case YOKE__INTERFACE__V1__CORE_FRAME__CARRIES_REFUSAL:
            refusal_into(f->refusal, err);
            result = YOKE_WAIT_FAILED;
            break;
        case YOKE__INTERFACE__V1__CORE_FRAME__CARRIES_COMPLETION:
            result = YOKE_WAIT_COMPLETE;
            break;
        case YOKE__INTERFACE__V1__CORE_FRAME__CARRIES_EVENT: {
            delivery = malloc(sizeof *delivery);
            yoke__interface__v1__subscribed__init(delivery);
            size_t len = protobuf_c_message_get_packed_size(&f->event->base);
            unsigned char *bytes = malloc(len + 1);
            protobuf_c_message_pack(&f->event->base, bytes);
            Yoke__Interface__V1__Event *event = yoke__interface__v1__event__unpack(NULL, len, bytes);
            free(bytes);
            delivery->carries_case = YOKE__INTERFACE__V1__SUBSCRIBED__CARRIES_EVENT;
            delivery->event = event;
            result = YOKE_WAIT_GOT;
            break;
        }
        case YOKE__INTERFACE__V1__CORE_FRAME__CARRIES_ANSWER:
            if (f->answer->answer_case == YOKE__INTERFACE__V1__RESPONSE__ANSWER_SUBSCRIBE) {
                size_t len = protobuf_c_message_get_packed_size(&f->answer->subscribe->base);
                unsigned char *bytes = malloc(len + 1);
                protobuf_c_message_pack(&f->answer->subscribe->base, bytes);
                delivery = yoke__interface__v1__subscribed__unpack(NULL, len, bytes);
                free(bytes);
                result = YOKE_WAIT_GOT;
            }
            break;
        default:
            break;
        }
        yoke__interface__v1__core_frame__free_unpacked(f, NULL);
        if (result != -2) {
            pthread_mutex_unlock(&a->mu);
            if (result == YOKE_WAIT_GOT) {
                *out = delivery;
            }
            return result;
        }
    }
}

void yoke_subscribed_free(Yoke__Interface__V1__Subscribed *delivery) {
    if (delivery != NULL) {
        yoke__interface__v1__subscribed__free_unpacked(delivery, NULL);
    }
}

void yoke_subscription_close(yoke_subscription *s) {
    if (s == NULL || s->standing) {
        return;
    }
    yoke_attachment *a = s->a;
    pthread_mutex_lock(&a->mu);
    if (!a->ended && !a->closing) {
        cancel_call(a, s->q);
    }
    forget_call(a, s->q);
    pthread_mutex_unlock(&a->mu);
    yoke_subscribed_free(s->pending);
    free(s->id);
    free(s);
}

/* ---- stream deliveries ---- */

void yoke_frame_clear(yoke_frame *frame) {
    if (frame != NULL) {
        free(frame->payload);
        memset(frame, 0, sizeof *frame);
    }
}

yoke_delivery *yoke_subscribe_stream(yoke_attachment *a, const char *unit, const char *stream,
                                     yoke_error *err) {
    Yoke__Interface__V1__Response *answer = NULL;
    if (unit_stream(a, YOKE__INTERFACE__V1__REQUEST__OPERATION_STREAM_SUBSCRIBE,
                    YOKE__INTERFACE__V1__RESPONSE__ANSWER_STREAM_SUBSCRIBE, unit, stream, &answer,
                    err) != 0) {
        return NULL;
    }
    Yoke__Interface__V1__Delivering *d = answer->stream_subscribe;
    yoke_delivery *out = calloc(1, sizeof *out);
    out->a = a;
    out->id = strdup(d->delivery);
    out->flowing = d->flowing;
    out->fd = -1;
    switch (d->arrives_case) {
    case YOKE__INTERFACE__V1__DELIVERING__ARRIVES_SOCKET: {
        struct sockaddr_un addr = {0};
        addr.sun_family = AF_UNIX;
        int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
        if (strlen(d->socket) >= sizeof addr.sun_path) {
            yoke_error_say(err, "the delivery's socket %s cannot be reached: the path is too long",
                           d->socket);
            close(fd);
            goto fail;
        }
        strcpy(addr.sun_path, d->socket);
        if (fd < 0 || connect(fd, (struct sockaddr *)&addr, sizeof addr) != 0) {
            yoke_error_say(err, "the delivery's socket %s cannot be reached: %s", d->socket, strerror(errno));
            if (fd >= 0) {
                close(fd);
            }
            goto fail;
        }
        out->fd = fd;
        break;
    }
    case YOKE__INTERFACE__V1__DELIVERING__ARRIVES_CONNECTION:
        pthread_mutex_lock(&a->mu);
        out->q = register_call(a, out->id);
        pthread_mutex_unlock(&a->mu);
        break;
    default:
        yoke_error_say(err, "the delivery %s arrives where this projection cannot read it", out->id);
        goto fail;
    }
    yoke_response_free(answer);
    return out;
fail:
    yoke_response_free(answer);
    free(out->id);
    free(out);
    return NULL;
}

const char *yoke_delivery_id(const yoke_delivery *d) { return d->id; }

int yoke_delivery_flowing(const yoke_delivery *d) { return d->flowing ? 1 : 0; }

int yoke_delivery_next(yoke_delivery *d, yoke_frame *frame, int timeout_ms, yoke_error *err) {
    memset(frame, 0, sizeof *frame);
    if (d->fd >= 0) {
        struct timespec by;
        if (timeout_ms >= 0) {
            deadline_in(timeout_ms, &by);
        }
        unsigned char *packet = malloc(PACKET);
        for (;;) {
            struct timespec now;
            int wait = -1;
            if (timeout_ms >= 0) {
                clock_gettime(CLOCK_REALTIME, &now);
                long long left =
                    ((long long)by.tv_sec - now.tv_sec) * 1000 + (by.tv_nsec - now.tv_nsec) / 1000000;
                wait = left < 0 ? 0 : (int)left;
            }
            struct timeval tv = {wait < 0 ? 0 : wait / 1000, wait < 0 ? 0 : (wait % 1000) * 1000};
            if (wait >= 0 && tv.tv_sec == 0 && tv.tv_usec == 0) {
                tv.tv_usec = 1;
            }
            setsockopt(d->fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
            ssize_t n = recv(d->fd, packet, PACKET, 0);
            if (n < 0 && errno == EINTR) {
                continue;
            }
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                free(packet);
                return YOKE_WAIT_TIMEOUT;
            }
            if (n <= 0) {
                free(packet);
                return YOKE_WAIT_COMPLETE;
            }
            if (n < HEADER) {
                free(packet);
                yoke_error_say(err, "a frame of %zd bytes is shorter than its header", n);
                return YOKE_WAIT_FAILED;
            }
            for (int i = 0; i < 8; i++) {
                frame->sequence |= (uint64_t)packet[i] << (8 * i);
                frame->sent_at |= (uint64_t)packet[8 + i] << (8 * i);
            }
            frame->payload_len = (size_t)n - HEADER;
            frame->payload = malloc(frame->payload_len + 1);
            memcpy(frame->payload, packet + HEADER, frame->payload_len);
            free(packet);
            return YOKE_WAIT_GOT;
        }
    }
    yoke_attachment *a = d->a;
    pthread_mutex_lock(&a->mu);
    for (;;) {
        bool late;
        Yoke__Interface__V1__CoreFrame *f = pop(a, d->q, timeout_ms, &late);
        if (f == NULL) {
            pthread_mutex_unlock(&a->mu);
            if (late) {
                return YOKE_WAIT_TIMEOUT;
            }
            ended_into(err);
            return YOKE_WAIT_FAILED;
        }
        int result = -2;
        if (f->carries_case == YOKE__INTERFACE__V1__CORE_FRAME__CARRIES_COMPLETION) {
            result = YOKE_WAIT_COMPLETE;
        } else if (f->carries_case == YOKE__INTERFACE__V1__CORE_FRAME__CARRIES_REFUSAL) {
            refusal_into(f->refusal, err);
            result = YOKE_WAIT_FAILED;
        } else if (f->carries_case == YOKE__INTERFACE__V1__CORE_FRAME__CARRIES_DELIVERY) {
            Yoke__Interface__V1__StreamDelivery *s = f->delivery;
            frame->sequence = s->sequence;
            frame->sent_at = s->sent_at;
            frame->payload_len = s->payload.len;
            frame->payload = malloc(s->payload.len + 1);
            if (s->payload.len > 0) {
                memcpy(frame->payload, s->payload.data, s->payload.len);
            }
            result = YOKE_WAIT_GOT;
        }
        yoke__interface__v1__core_frame__free_unpacked(f, NULL);
        if (result != -2) {
            pthread_mutex_unlock(&a->mu);
            return result;
        }
    }
}

int yoke_delivery_release(yoke_delivery *d, yoke_error *err) {
    yoke_attachment *a = d->a;
    Yoke__Interface__V1__StreamRelease release = YOKE__INTERFACE__V1__STREAM_RELEASE__INIT;
    release.delivery = d->id;
    Yoke__Interface__V1__Request r = YOKE__INTERFACE__V1__REQUEST__INIT;
    r.operation_case = YOKE__INTERFACE__V1__REQUEST__OPERATION_STREAM_UNSUBSCRIBE;
    r.stream_unsubscribe = &release;
    int result = call(a, &r, YOKE__INTERFACE__V1__RESPONSE__ANSWER_STREAM_UNSUBSCRIBE, NULL, err);
    if (d->fd >= 0) {
        close(d->fd);
    }
    if (d->q != NULL) {
        pthread_mutex_lock(&a->mu);
        forget_call(a, d->q);
        pthread_mutex_unlock(&a->mu);
    }
    free(d->id);
    free(d);
    return result;
}
