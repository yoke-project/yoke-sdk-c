#include "harness.h"

#include <errno.h>
#include <json-c/json.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

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

/* The harness's state: the control socket, the unit once started, and what it was sent to answer. */
typedef struct harness {
    int conn;
    pthread_mutex_t mu; /* the socket's writes, and the held events */
    yoke_unit *unit;
    yoke_event *held; /* commands and questions, kept for the ack or answer that names them */
    size_t held_len;
    int wake[2]; /* written when the Session ends, or the process is asked to stop */
    volatile bool ended;
} harness;

static int asked_pipe = -1;
static volatile sig_atomic_t asked;

static void on_term(int sig) {
    (void)sig;
    asked = 1;
    if (asked_pipe >= 0) {
        ssize_t ignored = write(asked_pipe, "", 1);
        (void)ignored;
    }
}

/* Writes one line: the object, then a newline. Takes ownership of the object. */
static void send_line(harness *h, json_object *o) {
    const char *s = json_object_to_json_string_ext(o, JSON_C_TO_STRING_PLAIN);
    pthread_mutex_lock(&h->mu);
    size_t len = strlen(s), off = 0;
    while (off < len) {
        ssize_t n = write(h->conn, s + off, len - off);
        if (n <= 0) {
            break;
        }
        off += (size_t)n;
    }
    ssize_t ignored = write(h->conn, "\n", 1);
    (void)ignored;
    pthread_mutex_unlock(&h->mu);
    json_object_put(o);
}

static json_object *strings(char **list) {
    json_object *a = json_object_new_array();
    for (size_t i = 0; list && list[i]; i++) {
        json_object_array_add(a, json_object_new_string(list[i]));
    }
    return a;
}

static json_object *scope(const yoke_scope *s) {
    json_object *o = json_object_new_object();
    json_object_object_add(o, "capabilities", strings(s->capabilities));
    json_object_object_add(o, "streams", strings(s->streams));
    json_object_object_add(o, "commands", strings(s->commands));
    json_object_object_add(o, "queries", strings(s->queries));
    return o;
}

static json_object *observation(const char *kind, json_object *fields) {
    json_object *o = json_object_new_object();
    json_object_object_add(o, "type", json_object_new_string("observation"));
    json_object_object_add(o, "kind", json_object_new_string(kind));
    json_object_object_add(o, "fields", fields);
    return o;
}

static void field(json_object *o, const char *name, const char *value) {
    json_object_object_add(o, name, json_object_new_string(value ? value : ""));
}

/* Reports everything the library surfaces, in the order it surfaced it. */
static void *observe(void *arg) {
    harness *h = arg;
    yoke_event e;
    while (yoke_next(h->unit, &e) == 1) {
        json_object *f = json_object_new_object();
        const char *kind = NULL;
        bool keep = false;
        switch (e.kind) {
        case YOKE_COMMAND:
            kind = "command", keep = true;
            field(f, "id", e.id);
            field(f, "type", e.type);
            break;
        case YOKE_QUESTION:
            kind = "question", keep = true;
            field(f, "id", e.id);
            field(f, "type", e.type);
            json_object_object_add(f, "payload",
                                   json_object_new_string_len((const char *)e.payload, (int)e.payload_len));
            break;
        case YOKE_ACTIVATED:
            kind = "activated";
            field(f, "stream", e.stream);
            field(f, "transport", e.transport);
            break;
        case YOKE_STOPPED:
            kind = "stopped";
            field(f, "stream", e.stream);
            break;
        case YOKE_REFUSED:
            kind = "refused";
            field(f, "correlation", e.correlation);
            field(f, "code", e.error.code);
            break;
        case YOKE_ENDED:
            kind = "session-ended";
            json_object_object_add(f, "closed", json_object_new_boolean(e.closed));
            field(f, "cause", e.cause);
            break;
        }
        if (kind) {
            send_line(h, observation(kind, f));
        } else {
            json_object_put(f);
        }
        bool end = e.kind == YOKE_ENDED;
        if (keep) {
            pthread_mutex_lock(&h->mu);
            yoke_event *grown = realloc(h->held, (h->held_len + 1) * sizeof *grown);
            if (grown) {
                h->held = grown;
                h->held[h->held_len++] = e;
            } else {
                yoke_event_clear(&e);
            }
            pthread_mutex_unlock(&h->mu);
        } else {
            yoke_event_clear(&e);
        }
        if (end) {
            break;
        }
    }
    // The Session ended: the incarnation is over, and so is the process.
    h->ended = true;
    ssize_t ignored = write(h->wake[1], "", 1);
    (void)ignored;
    return NULL;
}

/* The command or question the suite names, as the library handed it. */
static const yoke_event *held(harness *h, yoke_event_kind kind, const char *id) {
    const yoke_event *found = NULL;
    pthread_mutex_lock(&h->mu);
    for (size_t i = 0; id && i < h->held_len; i++) {
        if (h->held[i].kind == kind && h->held[i].id && strcmp(h->held[i].id, id) == 0) {
            found = &h->held[i];
        }
    }
    pthread_mutex_unlock(&h->mu);
    return found;
}

static const char *arg(json_object *args, const char *name) {
    json_object *v;
    return args && json_object_object_get_ex(args, name, &v) && json_object_is_type(v, json_type_string)
               ? json_object_get_string(v)
               : "";
}

/* A refusal is reported as its code, and never as the library's message. */
static void refusal(json_object *result, const yoke_error *err) {
    json_object *value = json_object_new_object();
    if (err->code) {
        json_object_object_add(result, "refusal", json_object_new_string(err->code));
        if (err->stage) {
            field(value, "stage", err->stage);
        }
    } else {
        json_object_object_add(value, "failed", json_object_new_boolean(true));
    }
    json_object_object_add(result, "value", value);
}

/* Turns a directive into a library call, and fills result with what the library returned. */
static void act(harness *h, const char *verb, json_object *args, yoke_getenv getenv, void *context,
                json_object *result) {
    yoke_error err = {0};
    json_object *value = json_object_new_object();
    if (strcmp(verb, "describe") == 0) {
        char *manifest = yoke_manifest(harness_declaration());
        field(value, "manifest", manifest);
        free(manifest);
        json_object_object_add(result, "value", value);
        return;
    }
    if (strcmp(verb, "start") == 0) {
        yoke_unit *u = yoke_start_with(harness_declaration(), getenv, context, &err);
        if (!u) {
            json_object_put(value);
            refusal(result, &err);
            yoke_error_clear(&err);
            return;
        }
        if (!h->unit) {
            h->unit = u;
            pthread_t t;
            pthread_create(&t, NULL, observe, h);
            pthread_detach(t);
        }
        const yoke_admission *a = yoke_admission_of(u);
        field(value, "outcome", a->restricted ? "accepted with restrictions" : "accepted");
        json_object_object_add(value, "granted", scope(&a->granted));
        json_object_object_add(value, "withheld", scope(&a->withheld));
        json_object_object_add(result, "value", value);
        return;
    }
    static const char *const acts[] = {"close", "emit", "report-health", "report", "ack", "answer", NULL};
    bool known = false;
    for (size_t i = 0; acts[i]; i++) {
        known = known || strcmp(verb, acts[i]) == 0;
    }
    if (!known) {
        json_object_put(value);
        json_object_object_add(result, "unrecognised", json_object_new_boolean(true));
        return;
    }
    if (!h->unit) {
        json_object_object_add(value, "failed", json_object_new_boolean(true));
        json_object_object_add(value, "started", json_object_new_boolean(false));
        json_object_object_add(result, "value", value);
        return;
    }
    int rc = 0;
    if (strcmp(verb, "close") == 0) {
        rc = yoke_close(h->unit, &err);
    } else if (strcmp(verb, "emit") == 0) {
        const char *payload = arg(args, "payload");
        rc = yoke_emit(h->unit, arg(args, "stream"), payload, strlen(payload), &err);
    } else if (strcmp(verb, "report-health") == 0) {
        json_object *g;
        unsigned grade = json_object_object_get_ex(args, "grade", &g) ? (unsigned)json_object_get_int(g) : 0;
        rc = yoke_health(h->unit, grade, arg(args, "line"), &err);
    } else if (strcmp(verb, "report") == 0) {
        json_object *n;
        yoke_severity severity = {0};
        if (json_object_object_get_ex(args, "severity", &n) &&
            (json_object_is_type(n, json_type_int) || json_object_is_type(n, json_type_double))) {
            severity = YOKE_SEVERITY((unsigned)json_object_get_int(n));
        }
        rc = yoke_report(h->unit, arg(args, "occurrence"), severity, arg(args, "line"), NULL, 0, &err);
    } else if (strcmp(verb, "ack") == 0) {
        yoke_event none = {.kind = YOKE_COMMAND, .id = (char *)""};
        const yoke_event *c = held(h, YOKE_COMMAND, arg(args, "command"));
        rc = yoke_ack(h->unit, c ? c : &none, YOKE_DONE, arg(args, "line"), &err);
    } else {
        const char *payload = arg(args, "payload");
        yoke_event none = {.kind = YOKE_QUESTION, .id = (char *)""};
        const yoke_event *q = held(h, YOKE_QUESTION, arg(args, "question"));
        rc = yoke_answer(h->unit, q ? q : &none, payload, strlen(payload), &err);
    }
    if (rc != 0) {
        json_object_put(value);
        refusal(result, &err);
        yoke_error_clear(&err);
        return;
    }
    json_object_object_add(result, "value", value);
}

/* Waits on the wake pipe for at most ms; whether it was woken. */
static bool woken(int fd, int ms) {
    struct pollfd p = {.fd = fd, .events = POLLIN};
    int n;
    do {
        n = poll(&p, 1, ms);
    } while (n < 0 && errno == EINTR);
    return n > 0;
}

int harness_serve(yoke_getenv getenv, void *context) {
    const char *path = getenv("CONFORMANCE_SOCKET", context);
    if (!path || strlen(path) >= sizeof((struct sockaddr_un){0}.sun_path)) {
        return 1;
    }
    harness h = {.conn = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0)};
    struct sockaddr_un addr = {.sun_family = AF_UNIX};
    strcpy(addr.sun_path, path);
    if (h.conn < 0 || connect(h.conn, (struct sockaddr *)&addr, sizeof addr) != 0 || pipe(h.wake) != 0) {
        return 1;
    }
    pthread_mutex_init(&h.mu, NULL);
    asked_pipe = h.wake[1];
    struct sigaction sa = {.sa_handler = on_term};
    sigaction(SIGTERM, &sa, NULL);

    json_object *hello = json_object_new_object();
    field(hello, "type", "hello");
    field(hello, "contract", "plugin");
    field(hello, "language", "c");
    field(hello, "sdk", YOKE_SDK_LINE);
    json_object_object_add(hello, "version", json_object_new_int(yoke_plugin_contract()));
    const char *unit = getenv("YOKE_UNIT", context);
    if (unit) {
        field(hello, "unit", unit);
    }
    send_line(&h, hello);

    char buf[65536];
    size_t have = 0;
    for (;;) {
        struct pollfd p[2] = {{.fd = h.conn, .events = POLLIN}, {.fd = h.wake[0], .events = POLLIN}};
        if (poll(p, 2, -1) < 0 && errno != EINTR) {
            return 1;
        }
        if (asked) {
            // Asked to stop, as the Core asks a process whose Session has ended: the end is reported
            // first, and the process leaves within the Core's window.
            for (long long until = time(NULL) + 2; !h.ended && time(NULL) < until;) {
                woken(h.wake[0], 100);
            }
            return 0;
        }
        if (h.ended) {
            return 0;
        }
        if (!(p[0].revents & (POLLIN | POLLHUP))) {
            continue;
        }
        ssize_t n = read(h.conn, buf + have, sizeof buf - have - 1);
        if (n <= 0) {
            if (h.unit) {
                yoke_close(h.unit, NULL);
            }
            return 0;
        }
        have += (size_t)n;
        char *start = buf, *nl;
        while ((nl = memchr(start, '\n', have - (size_t)(start - buf)))) {
            *nl = '\0';
            json_object *d = json_tokener_parse(start);
            start = nl + 1;
            json_object *type, *id, *verb, *args = NULL;
            if (!d || !json_object_object_get_ex(d, "type", &type)) {
                json_object_put(d);
                continue;
            }
            if (strcmp(json_object_get_string(type), "finish") == 0) {
                json_object_put(d);
                if (h.unit) {
                    yoke_close(h.unit, NULL);
                }
                return 0;
            }
            if (strcmp(json_object_get_string(type), "directive") != 0 ||
                !json_object_object_get_ex(d, "verb", &verb)) {
                json_object_put(d);
                continue;
            }
            json_object_object_get_ex(d, "args", &args);
            json_object *result = json_object_new_object();
            field(result, "type", "result");
            if (json_object_object_get_ex(d, "id", &id)) {
                field(result, "id", json_object_get_string(id));
            }
            act(&h, json_object_get_string(verb), args, getenv, context, result);
            send_line(&h, result);
            json_object_put(d);
        }
        have -= (size_t)(start - buf);
        memmove(buf, start, have);
        if (have == sizeof buf - 1) {
            have = 0; /* a line longer than any directive is dropped */
        }
    }
}
