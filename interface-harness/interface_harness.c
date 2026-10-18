#include "interface_harness.h"

#include <errno.h>
#include <json-c/json.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

/* What the harness holds: the control socket, and its attachments, the first being the one directed. */
typedef struct harness {
    int conn;
    pthread_mutex_t mu; /* the socket's writes */
    yoke_getenv getenv;
    void *context;
    yoke_attachment *attached[8];
    size_t attachments;
    char *standing;
    pthread_t threads[64];
    size_t running;
} harness;

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

static void field(json_object *o, const char *name, const char *value) {
    json_object_object_add(o, name, json_object_new_string(value ? value : ""));
}

/* A field left out when it is at its default, as the contract's own JSON mapping leaves it out. */
static void text(json_object *o, const char *name, const char *value) {
    if (value != NULL && value[0] != '\0') {
        field(o, name, value);
    }
}

static void flag(json_object *o, const char *name, bool value) {
    if (value) {
        json_object_object_add(o, name, json_object_new_boolean(true));
    }
}

/* A 64-bit number is written as a string, and zero is left out. */
static void big(json_object *o, const char *name, uint64_t n) {
    if (n != 0) {
        char digits[24];
        snprintf(digits, sizeof digits, "%llu", (unsigned long long)n);
        field(o, name, digits);
    }
}

static json_object *strings(char **list, size_t n) {
    json_object *a = json_object_new_array();
    for (size_t i = 0; i < n; i++) {
        json_object_array_add(a, json_object_new_string(list[i]));
    }
    return a;
}

/* A time as RFC 3339, in UTC. */
static json_object *rfc3339(const Google__Protobuf__Timestamp *t) {
    long long days = t->seconds / 86400, secs = t->seconds % 86400;
    if (secs < 0) {
        secs += 86400;
        days--;
    }
    long long z = days + 719468, era = (z >= 0 ? z : z - 146096) / 146097;
    long long doe = z - era * 146097, yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    long long doy = doe - (365 * yoe + yoe / 4 - yoe / 100), mp = (5 * doy + 2) / 153;
    long long day = doy - (153 * mp + 2) / 5 + 1, month = mp < 10 ? mp + 3 : mp - 9;
    long long year = yoe + era * 400 + (month <= 2);
    char out[64];
    int n = snprintf(out, sizeof out, "%04lld-%02lld-%02lldT%02lld:%02lld:%02lld", year, month, day,
                     secs / 3600, secs % 3600 / 60, secs % 60);
    if (t->nanos != 0) {
        char frac[16];
        snprintf(frac, sizeof frac, ".%09d", t->nanos);
        size_t len = strlen(frac);
        while (len > 1 && frac[len - 1] == '0') {
            frac[--len] = '\0';
        }
        n += snprintf(out + n, sizeof out - (size_t)n, "%s", frac);
    }
    snprintf(out + n, sizeof out - (size_t)n, "Z");
    return json_object_new_string(out);
}

static json_object *channel_record(const Yoke__Interface__V1__ChannelRecord *c) {
    json_object *v = json_object_new_object();
    if (c->declared != NULL) {
        json_object *d = json_object_new_object();
        text(d, "name", c->declared->name);
        text(d, "projection", c->declared->projection);
        text(d, "address_class", c->declared->address_class);
        text(d, "clients", c->declared->clients);
        json_object_object_add(v, "declared", d);
    }
    if (c->observed != NULL) {
        json_object *o = json_object_new_object();
        flag(o, "attached", c->observed->attached);
        text(o, "client", c->observed->client);
        flag(o, "suspended", c->observed->suspended);
        text(o, "grade", c->observed->grade);
        text(o, "by", c->observed->by);
        text(o, "reason", c->observed->reason);
        json_object_object_add(v, "observed", o);
    }
    return v;
}

static json_object *record(const Yoke__Interface__V1__Record *r) {
    json_object *o = json_object_new_object();
    switch (r->subject_case) {
    case YOKE__INTERFACE__V1__RECORD__SUBJECT_INSTANCE: {
        json_object *v = json_object_new_object();
        flag(v, "ready", r->instance->ready);
        flag(v, "stopping", r->instance->stopping);
        if (r->instance->since != NULL) {
            json_object_object_add(v, "since", rfc3339(r->instance->since));
        }
        json_object_object_add(o, "instance", v);
        break;
    }
    case YOKE__INTERFACE__V1__RECORD__SUBJECT_UNIT: {
        const Yoke__Interface__V1__UnitRecord *u = r->unit;
        json_object *v = json_object_new_object();
        if (u->declared != NULL) {
            json_object *d = json_object_new_object();
            text(d, "identity", u->declared->identity);
            text(d, "kind", u->declared->kind);
            json_object_object_add(v, "declared", d);
        }
        if (u->observed != NULL) {
            json_object *ob = json_object_new_object();
            text(ob, "state", u->observed->state);
            big(ob, "incarnation", u->observed->incarnation);
            if (u->observed->since != NULL) {
                json_object_object_add(ob, "since", rfc3339(u->observed->since));
            }
            if (u->observed->condition != NULL) {
                json_object *c = json_object_new_object();
                if (u->observed->condition->grade != 0) {
                    json_object_object_add(c, "grade",
                                           json_object_new_int((int)u->observed->condition->grade));
                }
                text(c, "line", u->observed->condition->line);
                if (u->observed->condition->since != NULL) {
                    json_object_object_add(c, "since", rfc3339(u->observed->condition->since));
                }
                json_object_object_add(ob, "condition", c);
            }
            if (u->observed->n_streams > 0) {
                json_object_object_add(ob, "streams", strings(u->observed->streams, u->observed->n_streams));
            }
            json_object_object_add(v, "observed", ob);
        }
        if (u->addressed != NULL) {
            json_object *ad = json_object_new_object();
            if (u->addressed->n_streams > 0) {
                json_object_object_add(ad, "streams",
                                       strings(u->addressed->streams, u->addressed->n_streams));
            }
            if (u->addressed->n_commands > 0) {
                json_object_object_add(ad, "commands",
                                       strings(u->addressed->commands, u->addressed->n_commands));
            }
            if (u->addressed->n_queries > 0) {
                json_object_object_add(ad, "queries",
                                       strings(u->addressed->queries, u->addressed->n_queries));
            }
            json_object_object_add(v, "addressed", ad);
        }
        json_object_object_add(o, "unit", v);
        break;
    }
    case YOKE__INTERFACE__V1__RECORD__SUBJECT_CHANNEL:
        json_object_object_add(o, "channel", channel_record(r->channel));
        break;
    default:
        break;
    }
    return o;
}

static json_object *records(Yoke__Interface__V1__Record **list, size_t n) {
    json_object *a = json_object_new_array();
    for (size_t i = 0; i < n; i++) {
        json_object_array_add(a, record(list[i]));
    }
    return a;
}

static json_object *acknowledged(const Yoke__Interface__V1__Acknowledged *k) {
    json_object *o = json_object_new_object();
    const ProtobufCEnumValue *v = protobuf_c_enum_descriptor_get_value(
        &yoke__interface__v1__acknowledged__outcome__descriptor, (int)k->outcome);
    if (k->outcome != 0 && v != NULL) {
        field(o, "outcome", v->name);
    }
    text(o, "line", k->line);
    return o;
}

static json_object *event_fields(const Yoke__Interface__V1__Event *e) {
    json_object *o = json_object_new_object();
    json_object_object_add(o, "seq", json_object_new_int64((int64_t)e->seq));
    field(o, "type", e->type);
    field(o, "subject_kind", e->subject ? e->subject->kind : "");
    field(o, "subject", e->subject ? e->subject->identity : "");
    json_object_object_add(o, "severity", json_object_new_int((int)e->severity));
    return o;
}

static const char *arg(json_object *args, const char *name) {
    json_object *v;
    return args && json_object_object_get_ex(args, name, &v) && json_object_is_type(v, json_type_string)
               ? json_object_get_string(v)
               : "";
}

/* A number the suite sent as a number, or as the string a 64-bit value is written as. */
static uint64_t number(json_object *args, const char *name) {
    json_object *v;
    if (!args || !json_object_object_get_ex(args, name, &v)) {
        return 0;
    }
    if (json_object_is_type(v, json_type_string)) {
        return strtoull(json_object_get_string(v), NULL, 10);
    }
    return (uint64_t)json_object_get_int64(v);
}

/* A refusal as its code and what it names; anything else the library failed with is a failure the harness
 * does not interpret. */
static void refusal(json_object *result, const yoke_error *err) {
    json_object *value = json_object_new_object();
    if (err->code == NULL) {
        json_object_object_add(value, "failed", json_object_new_boolean(true));
    } else {
        json_object_object_add(result, "refusal", json_object_new_string(err->code));
        if (err->subject.kind != NULL) {
            json_object *s = json_object_new_object();
            field(s, "kind", err->subject.kind);
            field(s, "identity", err->subject.identity);
            json_object_object_add(s, "incarnation",
                                   json_object_new_int64((int64_t)err->subject.incarnation));
            json_object_object_add(value, "subject", s);
        }
        if (err->item != NULL && err->item[0] != '\0') {
            field(value, "item", err->item);
        }
        if (err->suspension.grade != NULL) {
            json_object *s = json_object_new_object();
            field(s, "grade", err->suspension.grade);
            field(s, "by", err->suspension.by);
            json_object_object_add(value, "suspension", s);
        }
    }
    json_object_object_add(result, "value", value);
}

/* ---- what subscriptions and deliveries bring ---- */

typedef struct watch {
    harness *h;
    yoke_subscription *sub;
    yoke_delivery *delivery;
    bool standing;
} watch;

static void *watch_subscription(void *arg) {
    watch *w = arg;
    for (;;) {
        Yoke__Interface__V1__Subscribed *d = NULL;
        yoke_error err = {0};
        int got = yoke_subscription_next(w->sub, &d, -1, &err);
        yoke_error_clear(&err);
        if (got != YOKE_WAIT_GOT) {
            break;
        }
        json_object *fields = json_object_new_object();
        const char *kind = "";
        switch (d->carries_case) {
        case YOKE__INTERFACE__V1__SUBSCRIBED__CARRIES_SNAPSHOT:
        case YOKE__INTERFACE__V1__SUBSCRIBED__CARRIES_OVERFLOW: {
            const Yoke__Interface__V1__Snapshot *s =
                d->carries_case == YOKE__INTERFACE__V1__SUBSCRIBED__CARRIES_SNAPSHOT ? d->snapshot
                                                                                     : d->overflow;
            kind = d->carries_case == YOKE__INTERFACE__V1__SUBSCRIBED__CARRIES_SNAPSHOT ? "snapshot"
                                                                                        : "overflow";
            json_object_object_add(fields, "at", json_object_new_int64((int64_t)s->at));
            json_object_object_add(fields, "records", records(s->records, s->n_records));
            break;
        }
        case YOKE__INTERFACE__V1__SUBSCRIBED__CARRIES_EVENT:
            kind = "event";
            json_object_put(fields);
            fields = event_fields(d->event);
            break;
        default:
            break;
        }
        json_object *o = json_object_new_object();
        field(o, "type", "observation");
        field(o, "kind", kind);
        json_object_object_add(o, "fields", fields);
        send_line(w->h, o);
        yoke_subscribed_free(d);
    }
    json_object *o = json_object_new_object();
    field(o, "type", "observation");
    field(o, "kind", "ended");
    json_object_object_add(o, "fields", json_object_new_object());
    send_line(w->h, o);
    if (!w->standing) {
        yoke_subscription_close(w->sub);
    }
    free(w);
    return NULL;
}

static void *watch_delivery(void *arg) {
    watch *w = arg;
    char *id = strdup(yoke_delivery_id(w->delivery));
    for (;;) {
        yoke_frame frame;
        yoke_error err = {0};
        int got = yoke_delivery_next(w->delivery, &frame, -1, &err);
        yoke_error_clear(&err);
        if (got != YOKE_WAIT_GOT) {
            break;
        }
        json_object *fields = json_object_new_object();
        field(fields, "delivery", id);
        json_object_object_add(fields, "sequence", json_object_new_int64((int64_t)frame.sequence));
        json_object_object_add(fields, "sent_at", json_object_new_int64((int64_t)frame.sent_at));
        json_object_object_add(
            fields, "payload",
            json_object_new_string_len((const char *)frame.payload, (int)frame.payload_len));
        json_object *o = json_object_new_object();
        field(o, "type", "observation");
        field(o, "kind", "frame");
        json_object_object_add(o, "fields", fields);
        send_line(w->h, o);
        yoke_frame_clear(&frame);
    }
    json_object *fields = json_object_new_object();
    field(fields, "delivery", id);
    json_object *o = json_object_new_object();
    field(o, "type", "observation");
    field(o, "kind", "delivery-ended");
    json_object_object_add(o, "fields", fields);
    send_line(w->h, o);
    yoke_delivery_release(w->delivery, NULL);
    free(id);
    free(w);
    return NULL;
}

static void watching(harness *h, void *(*run)(void *), watch *w) {
    if (h->running < sizeof h->threads / sizeof h->threads[0]) {
        pthread_create(&h->threads[h->running++], NULL, run, w);
    } else {
        free(w);
    }
}

/* ---- directives ---- */

static void attach(harness *h, const char *channel, json_object *result) {
    yoke_error err = {0};
    char *address = NULL;
    const char *instance = h->getenv("CONFORMANCE_INSTANCE", h->context);
    if (yoke_interface_service(&address, instance ? instance : "", channel, &err) != 0) {
        json_object *value = json_object_new_object();
        json_object_object_add(value, "failed", json_object_new_boolean(true));
        json_object_object_add(result, "value", value);
        yoke_error_clear(&err);
        return;
    }
    yoke_attach_options options = {.timeout_ms = 30000};
    yoke_attachment *a = yoke_attach(address, &options, &err);
    free(address);
    if (a == NULL) {
        refusal(result, &err);
        yoke_error_clear(&err);
        return;
    }
    yoke_subscription *standing = yoke_standing(a);
    json_object *value = json_object_new_object();
    json_object_object_add(value, "version", json_object_new_int(yoke_interface_version(a)));
    field(value, "subscription", standing ? yoke_subscription_id(standing) : "");
    json_object_object_add(value, "at", json_object_new_int64((int64_t)yoke_picture(a)->at));
    json_object_object_add(value, "records", records(yoke_picture(a)->records, yoke_picture(a)->n_records));
    json_object_object_add(result, "value", value);
    /* A second attachment the channel admitted is kept beside the first, which stays the one directed. */
    if (h->attachments < sizeof h->attached / sizeof h->attached[0]) {
        h->attached[h->attachments++] = a;
    }
    if (h->attachments == 1) {
        h->standing = standing ? strdup(yoke_subscription_id(standing)) : NULL;
        if (standing != NULL) {
            watch *w = calloc(1, sizeof *w);
            w->h = h;
            w->sub = standing;
            w->standing = true;
            watching(h, watch_subscription, w);
        }
    }
}

static void answered(json_object *result, Yoke__Interface__V1__Response *answer, json_object *value) {
    yoke_response_free(answer);
    json_object_object_add(result, "value", value);
}

static void act(harness *h, const char *verb, json_object *args, json_object *result) {
    yoke_error err = {0};
    static const char *const known[] = {
        "authenticate", "read",        "subscribe",        "confirm", "command", "query",
        "stream-start", "stream-stop", "stream-subscribe", "reclaim", NULL};
    if (strcmp(verb, "attach") == 0) {
        attach(h, arg(args, "channel"), result);
        return;
    }
    bool ok = false;
    for (size_t i = 0; known[i]; i++) {
        ok = ok || strcmp(verb, known[i]) == 0;
    }
    if (!ok) {
        json_object_object_add(result, "unrecognised", json_object_new_boolean(true));
        return;
    }
    if (h->attachments == 0) {
        json_object *value = json_object_new_object();
        json_object_object_add(value, "failed", json_object_new_boolean(true));
        json_object_object_add(value, "attached", json_object_new_boolean(false));
        json_object_object_add(result, "value", value);
        return;
    }
    yoke_attachment *a = h->attached[0];
    Yoke__Interface__V1__Response *answer = NULL;
    int rc = 0;
    json_object *value = NULL;
    if (strcmp(verb, "authenticate") == 0) {
        const char *token = arg(args, "token");
        rc = token[0] ? yoke_authenticate_token(a, token, &answer, &err)
                      : yoke_authenticate(a, arg(args, "account"), arg(args, "secret"), &answer, &err);
        if (rc == 0) {
            value = json_object_new_object();
            text(value, "account", answer->authenticate->account);
            text(value, "token", answer->authenticate->token);
            if (answer->authenticate->expires != NULL) {
                json_object_object_add(value, "expires", rfc3339(answer->authenticate->expires));
            }
        }
    } else if (strcmp(verb, "read") == 0) {
        rc = yoke_read(a, arg(args, "kind"), arg(args, "identity"), &answer, &err);
        if (rc == 0) {
            value = json_object_new_object();
            json_object_object_add(value, "records", records(answer->read->records, answer->read->n_records));
        }
    } else if (strcmp(verb, "subscribe") == 0) {
        Yoke__Interface__V1__Filter filter = YOKE__INTERFACE__V1__FILTER__INIT;
        filter.subject_kind = (char *)arg(args, "subject_kind");
        filter.subject_identity = (char *)arg(args, "subject_identity");
        filter.type = (char *)arg(args, "type");
        yoke_subscription *s = yoke_subscribe(a, &filter, &err);
        if (s == NULL) {
            rc = -1;
        } else {
            watch *w = calloc(1, sizeof *w);
            w->h = h;
            w->sub = s;
            watching(h, watch_subscription, w);
            value = json_object_new_object();
        }
    } else if (strcmp(verb, "confirm") == 0) {
        const char *subscription = arg(args, "subscription");
        rc = yoke_confirm(a, subscription[0] ? subscription : (h->standing ? h->standing : ""),
                          number(args, "sequence"), &answer, &err);
        if (rc == 0) {
            value = json_object_new_object();
        }
    } else if (strcmp(verb, "command") == 0 || strcmp(verb, "query") == 0) {
        const char *payload = arg(args, "payload");
        bool command = strcmp(verb, "command") == 0;
        rc = command
                 ? yoke_command(a, arg(args, "unit"), arg(args, "type"), payload, strlen(payload), &answer,
                                &err)
                 : yoke_ask(a, arg(args, "unit"), arg(args, "type"), payload, strlen(payload), &answer, &err);
        if (rc == 0) {
            if (command) {
                value = acknowledged(answer->command);
            } else {
                value = json_object_new_object();
                json_object_object_add(value, "answer",
                                       json_object_new_string_len((const char *)answer->query->payload.data,
                                                                  (int)answer->query->payload.len));
            }
        }
    } else if (strcmp(verb, "stream-start") == 0 || strcmp(verb, "stream-stop") == 0) {
        bool start = strcmp(verb, "stream-start") == 0;
        rc = start ? yoke_start_stream(a, arg(args, "unit"), arg(args, "stream"), &answer, &err)
                   : yoke_stop_stream(a, arg(args, "unit"), arg(args, "stream"), &answer, &err);
        if (rc == 0) {
            value = acknowledged(start ? answer->stream_start : answer->stream_stop);
        }
    } else if (strcmp(verb, "reclaim") == 0) {
        rc = yoke_reclaim(a, &answer, &err);
        if (rc == 0) {
            value = json_object_new_object();
            if (answer->reclaim->channel != NULL) {
                json_object_object_add(value, "channel", channel_record(answer->reclaim->channel));
            }
            flag(value, "changed", answer->reclaim->changed);
        }
    } else {
        yoke_delivery *d = yoke_subscribe_stream(a, arg(args, "unit"), arg(args, "stream"), &err);
        if (d == NULL) {
            rc = -1;
        } else {
            value = json_object_new_object();
            field(value, "delivery", yoke_delivery_id(d));
            json_object_object_add(value, "flowing", json_object_new_boolean(yoke_delivery_flowing(d)));
            watch *w = calloc(1, sizeof *w);
            w->h = h;
            w->delivery = d;
            watching(h, watch_delivery, w);
        }
    }
    if (rc != 0) {
        refusal(result, &err);
        yoke_error_clear(&err);
        return;
    }
    answered(result, answer, value);
}

int interface_harness_serve(yoke_getenv getenv, void *context) {
    const char *path = getenv("CONFORMANCE_SOCKET", context);
    if (!path || strlen(path) >= sizeof((struct sockaddr_un){0}.sun_path)) {
        return 1;
    }
    harness h = {
        .conn = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0), .getenv = getenv, .context = context};
    struct sockaddr_un addr = {.sun_family = AF_UNIX};
    strcpy(addr.sun_path, path);
    if (h.conn < 0 || connect(h.conn, (struct sockaddr *)&addr, sizeof addr) != 0) {
        return 1;
    }
    pthread_mutex_init(&h.mu, NULL);

    json_object *hello = json_object_new_object();
    field(hello, "type", "hello");
    field(hello, "contract", "interface");
    field(hello, "language", "c");
    field(hello, "sdk", YOKE_SDK_LINE);
    json_object_object_add(hello, "version", json_object_new_int(yoke_interface_contract()));
    send_line(&h, hello);

    char buf[65536];
    size_t have = 0;
    bool finishing = false;
    while (!finishing) {
        ssize_t n = read(h.conn, buf + have, sizeof buf - have - 1);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            break;
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
                finishing = true;
                break;
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
            act(&h, json_object_get_string(verb), args, result);
            send_line(&h, result);
            json_object_put(d);
        }
        if (finishing) {
            break;
        }
        have -= (size_t)(start - buf);
        memmove(buf, start, have);
        if (have == sizeof buf - 1) {
            have = 0; /* a line longer than any directive is dropped */
        }
    }
    /* Every attachment is ended in order, which lets the threads reading it go; then it is let go. */
    for (size_t i = 0; i < h.attachments; i++) {
        yoke_hangup(h.attached[i]);
    }
    for (size_t i = 0; i < h.running; i++) {
        pthread_join(h.threads[i], NULL);
    }
    for (size_t i = 0; i < h.attachments; i++) {
        yoke_detach(h.attached[i]);
    }
    free(h.standing);
    close(h.conn);
    return 0;
}
