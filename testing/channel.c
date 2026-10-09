#define _GNU_SOURCE
#include "channel.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include <grpc/byte_buffer_reader.h>
#include <grpc/grpc.h>
#include <grpc/grpc_security.h>
#include <grpc/support/alloc.h>

#include "check.h"

/* What a completion is about. */
typedef enum {
    NEW_CALL,
    REGISTRATION_READ,
    REGISTRATION_ANSWERED,
    SESSION_METADATA,
    SESSION_READ,
    SESSION_WRITTEN,
    SESSION_OVER,
    SHUTDOWN,
} what;

typedef struct tag {
    what what;
} tag;

/* One write the Session waits to make: a message, or the status that ends the stream. */
typedef struct write {
    grpc_byte_buffer *message; /* NULL: the status */
    struct write *next;
} write;

struct channel {
    char socket[256], bind[256];
    channel_terms terms;
    grpc_server *server;
    grpc_completion_queue *cq;
    pthread_t thread;

    pthread_mutex_t mu;
    pthread_cond_t changed;

    /* The call being offered. */
    grpc_call *offered;
    grpc_call_details details;
    grpc_metadata_array offered_metadata;

    /* The registration being answered, one at a time. */
    grpc_call *registering;
    grpc_byte_buffer *registration;
    grpc_byte_buffer *answer;
    int cancelled;
    int registrations;
    Yoke__Plugin__V1__RegisterRequest *request;
    bool bound;

    /* The Session. */
    grpc_call *session;
    int pending; /* batches the Session's call has outstanding */
    bool over;
    bool half_closed;
    int sessions;
    grpc_byte_buffer *incoming;
    int session_cancelled;
    write *writes, *last;
    bool writing;
    grpc_byte_buffer *in_flight; /* kept until its write completes, as gRPC requires */
    unsigned long long next;

    Yoke__Plugin__V1__Envelope **arrivals;
    long long *at;
    size_t count, cap;

    tag tags[SHUTDOWN + 1];
};

static void offer(channel *ch) {
    grpc_call_details_init(&ch->details);
    grpc_metadata_array_init(&ch->offered_metadata);
    grpc_server_request_call(ch->server, &ch->offered, &ch->details, &ch->offered_metadata, ch->cq, ch->cq,
                             &ch->tags[NEW_CALL]);
}

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

/* Starts the next write, if there is one and none is outstanding. Holds mu. */
static void start_write(channel *ch) {
    if (ch->writing || ch->writes == NULL || ch->session == NULL || ch->over) {
        return;
    }
    write *w = ch->writes;
    ch->writes = w->next;
    if (ch->writes == NULL) {
        ch->last = NULL;
    }
    grpc_op op = {0};
    if (w->message != NULL) {
        op.op = GRPC_OP_SEND_MESSAGE;
        op.data.send_message.send_message = w->message;
    } else {
        op.op = GRPC_OP_SEND_STATUS_FROM_SERVER;
        op.data.send_status_from_server.status = GRPC_STATUS_OK;
    }
    ch->writing = true;
    ch->in_flight = w->message;
    ch->pending++;
    grpc_call_start_batch(ch->session, &op, 1, &ch->tags[SESSION_WRITTEN], NULL);
    free(w);
}

static void read_session(channel *ch) {
    grpc_op op = {.op = GRPC_OP_RECV_MESSAGE};
    op.data.recv_message.recv_message = &ch->incoming;
    ch->pending++;
    grpc_call_start_batch(ch->session, &op, 1, &ch->tags[SESSION_READ], NULL);
}

/* Lets the Session's call go once nothing is outstanding on it. Holds mu. */
static void release_session(channel *ch) {
    if (ch->session != NULL && ch->over && ch->pending == 0) {
        grpc_call_unref(ch->session);
        ch->session = NULL;
        ch->writing = false;
        while (ch->writes != NULL) {
            write *w = ch->writes;
            ch->writes = w->next;
            if (w->message != NULL) {
                grpc_byte_buffer_destroy(w->message);
            }
            free(w);
        }
        ch->last = NULL;
    }
}

static void answer_registration(channel *ch) {
    size_t len;
    unsigned char *bytes = bytes_of(ch->registration, &len);
    grpc_byte_buffer_destroy(ch->registration);
    ch->registration = NULL;
    Yoke__Plugin__V1__RegisterRequest *request = yoke__plugin__v1__register_request__unpack(NULL, len, bytes);
    free(bytes);
    struct stat st;
    bool bound = stat(ch->bind, &st) == 0 && S_ISSOCK(st.st_mode);

    pthread_mutex_lock(&ch->mu);
    ch->registrations++;
    if (ch->request == NULL) {
        ch->request = request;
        ch->bound = bound;
    } else if (request != NULL) {
        yoke__plugin__v1__register_request__free_unpacked(request, NULL);
    }
    pthread_cond_broadcast(&ch->changed);
    pthread_mutex_unlock(&ch->mu);

    const channel_terms *t = &ch->terms;
    Yoke__Plugin__V1__RegisterResponse response = YOKE__PLUGIN__V1__REGISTER_RESPONSE__INIT;
    Yoke__Plugin__V1__HeartbeatTerms heartbeat = YOKE__PLUGIN__V1__HEARTBEAT_TERMS__INIT;
    Google__Protobuf__Duration interval = GOOGLE__PROTOBUF__DURATION__INIT;
    response.outcome =
        t->outcome ? t->outcome : YOKE__PLUGIN__V1__REGISTER_RESPONSE__OUTCOME__OUTCOME_ACCEPTED;
    response.stage = t->stage;
    response.code = (char *)(t->code ? t->code : "");
    response.message = (char *)(t->message ? t->message : "");
    if (response.outcome != YOKE__PLUGIN__V1__REGISTER_RESPONSE__OUTCOME__OUTCOME_REFUSED) {
        response.session_id = (char *)(t->session ? t->session : "s-1");
        response.granted = t->granted;
        response.withheld = t->withheld;
        if (t->interval_ms > 0) {
            interval.seconds = t->interval_ms / 1000;
            interval.nanos = (t->interval_ms % 1000) * 1000000;
            heartbeat.interval = &interval;
            heartbeat.tolerance = 3;
            response.heartbeat = &heartbeat;
        }
    }
    grpc_op ops[3] = {0};
    ops[0].op = GRPC_OP_SEND_MESSAGE;
    ch->answer = pack(&response.base);
    ops[0].data.send_message.send_message = ch->answer;
    ops[1].op = GRPC_OP_SEND_STATUS_FROM_SERVER;
    ops[1].data.send_status_from_server.status = GRPC_STATUS_OK;
    ops[2].op = GRPC_OP_RECV_CLOSE_ON_SERVER;
    ops[2].data.recv_close_on_server.cancelled = &ch->cancelled;
    grpc_call_start_batch(ch->registering, ops, 3, &ch->tags[REGISTRATION_ANSWERED], NULL);
}

static void accept_call(channel *ch) {
    char *method = grpc_slice_to_c_string(ch->details.method);
    grpc_call *call = ch->offered;
    grpc_call_details_destroy(&ch->details);
    grpc_metadata_array_destroy(&ch->offered_metadata);

    if (strcmp(method, "/yoke.plugin.v1.Register/Register") == 0 && ch->registering == NULL) {
        ch->registering = call;
        grpc_op ops[2] = {0};
        ops[0].op = GRPC_OP_SEND_INITIAL_METADATA;
        ops[1].op = GRPC_OP_RECV_MESSAGE;
        ops[1].data.recv_message.recv_message = &ch->registration;
        grpc_call_start_batch(call, ops, 2, &ch->tags[REGISTRATION_READ], NULL);
    } else if (strcmp(method, "/yoke.plugin.v1.Session/Open") == 0) {
        pthread_mutex_lock(&ch->mu);
        ch->sessions++;
        bool first = ch->session == NULL;
        if (first) {
            ch->session = call;
            ch->over = false;
            ch->half_closed = false;
        }
        pthread_cond_broadcast(&ch->changed);
        if (first) {
            grpc_op meta = {.op = GRPC_OP_SEND_INITIAL_METADATA};
            ch->pending += 2;
            grpc_call_start_batch(call, &meta, 1, &ch->tags[SESSION_METADATA], NULL);
            grpc_op close = {.op = GRPC_OP_RECV_CLOSE_ON_SERVER};
            close.data.recv_close_on_server.cancelled = &ch->session_cancelled;
            grpc_call_start_batch(call, &close, 1, &ch->tags[SESSION_OVER], NULL);
            read_session(ch);
            start_write(ch);
        }
        pthread_mutex_unlock(&ch->mu);
        if (!first) {
            grpc_call_cancel(call, NULL);
            grpc_call_unref(call);
        }
    } else {
        grpc_call_cancel(call, NULL);
        grpc_call_unref(call);
    }
    gpr_free(method);
}

static void *serve(void *arg) {
    channel *ch = arg;
    for (;;) {
        grpc_event ev = grpc_completion_queue_next(ch->cq, gpr_inf_future(GPR_CLOCK_REALTIME), NULL);
        if (ev.type == GRPC_QUEUE_SHUTDOWN) {
            return NULL;
        }
        tag *t = ev.tag;
        switch (t->what) {
        case NEW_CALL:
            if (!ev.success) {
                break; /* the server is shutting down */
            }
            accept_call(ch);
            offer(ch);
            break;
        case REGISTRATION_READ:
            if (ev.success && ch->registration != NULL) {
                answer_registration(ch);
            } else {
                grpc_call_cancel(ch->registering, NULL);
                grpc_call_unref(ch->registering);
                ch->registering = NULL;
            }
            break;
        case REGISTRATION_ANSWERED:
            grpc_byte_buffer_destroy(ch->answer);
            ch->answer = NULL;
            grpc_call_unref(ch->registering);
            ch->registering = NULL;
            break;
        case SESSION_METADATA:
            pthread_mutex_lock(&ch->mu);
            ch->pending--;
            release_session(ch);
            pthread_mutex_unlock(&ch->mu);
            break;
        case SESSION_READ: {
            pthread_mutex_lock(&ch->mu);
            ch->pending--;
            if (ev.success && ch->incoming != NULL) {
                size_t len;
                unsigned char *bytes = bytes_of(ch->incoming, &len);
                grpc_byte_buffer_destroy(ch->incoming);
                ch->incoming = NULL;
                Yoke__Plugin__V1__Envelope *e = yoke__plugin__v1__envelope__unpack(NULL, len, bytes);
                free(bytes);
                if (ch->count == ch->cap) {
                    ch->cap = ch->cap ? ch->cap * 2 : 64;
                    ch->arrivals = realloc(ch->arrivals, ch->cap * sizeof *ch->arrivals);
                    ch->at = realloc(ch->at, ch->cap * sizeof *ch->at);
                }
                ch->arrivals[ch->count] = e;
                ch->at[ch->count] = check_ms();
                ch->count++;
                if (!ch->over) {
                    read_session(ch);
                }
            } else {
                ch->half_closed = true;
            }
            pthread_cond_broadcast(&ch->changed);
            release_session(ch);
            pthread_mutex_unlock(&ch->mu);
            break;
        }
        case SESSION_WRITTEN:
            pthread_mutex_lock(&ch->mu);
            ch->pending--;
            ch->writing = false;
            if (ch->in_flight != NULL) {
                grpc_byte_buffer_destroy(ch->in_flight);
                ch->in_flight = NULL;
            }
            start_write(ch);
            release_session(ch);
            pthread_mutex_unlock(&ch->mu);
            break;
        case SESSION_OVER:
            pthread_mutex_lock(&ch->mu);
            ch->pending--;
            ch->over = true;
            pthread_cond_broadcast(&ch->changed);
            release_session(ch);
            pthread_mutex_unlock(&ch->mu);
            break;
        case SHUTDOWN:
            grpc_completion_queue_shutdown(ch->cq);
            break;
        }
    }
}

channel *channel_start(const char *dir, const channel_terms *terms) {
    grpc_init();
    channel *ch = calloc(1, sizeof *ch);
    for (int i = 0; i <= SHUTDOWN; i++) {
        ch->tags[i].what = (what)i;
    }
    snprintf(ch->socket, sizeof ch->socket, "%s/plugin.sock", dir);
    snprintf(ch->bind, sizeof ch->bind, "%s/unit.sock", dir);
    if (terms != NULL) {
        ch->terms = *terms;
    }
    pthread_mutex_init(&ch->mu, NULL);
    pthread_cond_init(&ch->changed, NULL);
    ch->cq = grpc_completion_queue_create_for_next(NULL);
    ch->server = grpc_server_create(NULL, NULL);
    grpc_server_register_completion_queue(ch->server, ch->cq, NULL);
    char address[300];
    snprintf(address, sizeof address, "unix:%s", ch->socket);
    grpc_server_credentials *credentials = grpc_insecure_server_credentials_create();
    if (!grpc_server_add_http2_port(ch->server, address, credentials)) {
        fprintf(stderr, "channel: cannot listen at %s\n", address);
        abort();
    }
    grpc_server_credentials_release(credentials);
    grpc_server_start(ch->server);
    offer(ch);
    pthread_create(&ch->thread, NULL, serve, ch);
    return ch;
}

void channel_stop(channel *ch) {
    grpc_server_shutdown_and_notify(ch->server, ch->cq, &ch->tags[SHUTDOWN]);
    grpc_server_cancel_all_calls(ch->server);
    pthread_join(ch->thread, NULL);
    grpc_server_destroy(ch->server);
    grpc_completion_queue_destroy(ch->cq);
    if (ch->session != NULL) {
        grpc_call_unref(ch->session);
    }
    for (size_t i = 0; i < ch->count; i++) {
        yoke__plugin__v1__envelope__free_unpacked(ch->arrivals[i], NULL);
    }
    free(ch->arrivals);
    free(ch->at);
    if (ch->request != NULL) {
        yoke__plugin__v1__register_request__free_unpacked(ch->request, NULL);
    }
    while (ch->writes != NULL) {
        write *w = ch->writes;
        ch->writes = w->next;
        if (w->message != NULL) {
            grpc_byte_buffer_destroy(w->message);
        }
        free(w);
    }
    pthread_mutex_destroy(&ch->mu);
    pthread_cond_destroy(&ch->changed);
    free(ch);
    grpc_shutdown();
}

const char *channel_socket(const channel *ch) { return ch->socket; }

const char *channel_bind(const channel *ch) { return ch->bind; }

const char *channel_getenv(const char *name, void *arg) {
    channel *ch = arg;
    if (strcmp(name, "YOKE_PLUGIN") == 0) {
        return "com.yoke.station.acquire";
    }
    if (strcmp(name, "YOKE_UNIT") == 0) {
        return "acquire";
    }
    if (strcmp(name, "YOKE_SOCKET") == 0) {
        return ch->socket;
    }
    if (strcmp(name, "YOKE_BIND") == 0) {
        return ch->bind;
    }
    if (strcmp(name, "YOKE_TOKEN") == 0) {
        return "t-0123";
    }
    return NULL;
}

int channel_registrations(channel *ch) {
    pthread_mutex_lock(&ch->mu);
    int n = ch->registrations;
    pthread_mutex_unlock(&ch->mu);
    return n;
}

int channel_sessions(channel *ch) {
    pthread_mutex_lock(&ch->mu);
    int n = ch->sessions;
    pthread_mutex_unlock(&ch->mu);
    return n;
}

const Yoke__Plugin__V1__RegisterRequest *channel_request(channel *ch) {
    pthread_mutex_lock(&ch->mu);
    const Yoke__Plugin__V1__RegisterRequest *r = ch->request;
    pthread_mutex_unlock(&ch->mu);
    return r;
}

bool channel_bound_at_registration(channel *ch) {
    pthread_mutex_lock(&ch->mu);
    bool b = ch->bound;
    pthread_mutex_unlock(&ch->mu);
    return b;
}

size_t channel_count(channel *ch) {
    pthread_mutex_lock(&ch->mu);
    size_t n = ch->count;
    pthread_mutex_unlock(&ch->mu);
    return n;
}

const Yoke__Plugin__V1__Envelope *channel_arrival(channel *ch, size_t i, long long *at) {
    pthread_mutex_lock(&ch->mu);
    const Yoke__Plugin__V1__Envelope *e = i < ch->count ? ch->arrivals[i] : NULL;
    if (e != NULL && at != NULL) {
        *at = ch->at[i];
    }
    pthread_mutex_unlock(&ch->mu);
    return e;
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

long channel_await(channel *ch, Yoke__Plugin__V1__Envelope__PayloadCase payload, size_t from,
                   int timeout_ms) {
    struct timespec deadline = deadline_in(timeout_ms);
    pthread_mutex_lock(&ch->mu);
    size_t i = from;
    for (;;) {
        for (; i < ch->count; i++) {
            if (ch->arrivals[i] != NULL && ch->arrivals[i]->payload_case == payload) {
                pthread_mutex_unlock(&ch->mu);
                return (long)i;
            }
        }
        if (pthread_cond_timedwait(&ch->changed, &ch->mu, &deadline) != 0) {
            pthread_mutex_unlock(&ch->mu);
            return -1;
        }
    }
}

static void enqueue(channel *ch, grpc_byte_buffer *message) {
    write *w = calloc(1, sizeof *w);
    w->message = message;
    pthread_mutex_lock(&ch->mu);
    if (ch->last != NULL) {
        ch->last->next = w;
    } else {
        ch->writes = w;
    }
    ch->last = w;
    start_write(ch);
    pthread_mutex_unlock(&ch->mu);
}

void channel_send(channel *ch, Yoke__Plugin__V1__Envelope *e, char id[32]) {
    pthread_mutex_lock(&ch->mu);
    snprintf(id, 32, "c-%llu", ++ch->next);
    pthread_mutex_unlock(&ch->mu);
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    e->message_id = id;
    e->session_id = (char *)(ch->terms.session ? ch->terms.session : "s-1");
    e->sent_at_unix_nano = (long long)now.tv_sec * 1000000000 + now.tv_nsec;
    enqueue(ch, pack(&e->base));
}

void channel_end(channel *ch) { enqueue(ch, NULL); }

bool channel_await_half_close(channel *ch, int timeout_ms) {
    struct timespec deadline = deadline_in(timeout_ms);
    pthread_mutex_lock(&ch->mu);
    while (!ch->half_closed) {
        if (pthread_cond_timedwait(&ch->changed, &ch->mu, &deadline) != 0) {
            break;
        }
    }
    bool closed = ch->half_closed;
    pthread_mutex_unlock(&ch->mu);
    return closed;
}
