#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include "check.h"
#include "core.h"
#include "yoke/interface.h"

/* ---- frames a script says ---- */

static void say_answer(core_conn *c, const char *call, Yoke__Interface__V1__Response *r) {
    Yoke__Interface__V1__CoreFrame f = YOKE__INTERFACE__V1__CORE_FRAME__INIT;
    f.call = (char *)call;
    f.carries_case = YOKE__INTERFACE__V1__CORE_FRAME__CARRIES_ANSWER;
    f.answer = r;
    core_send(c, &f);
}

static void say_event(core_conn *c, const char *call, uint64_t seq) {
    Yoke__Interface__V1__Event e = YOKE__INTERFACE__V1__EVENT__INIT;
    e.seq = seq;
    e.type = (char *)"unit.state.changed";
    Yoke__Interface__V1__CoreFrame f = YOKE__INTERFACE__V1__CORE_FRAME__INIT;
    f.call = (char *)call;
    f.carries_case = YOKE__INTERFACE__V1__CORE_FRAME__CARRIES_EVENT;
    f.event = &e;
    core_send(c, &f);
}

static void say_refusal(core_conn *c, const char *call, Yoke__Interface__V1__Refusal *r) {
    core_send(c, core_refusal_frame(call, r));
}

static void say_completion(core_conn *c, const char *call) {
    Yoke__Interface__V1__Completion done = YOKE__INTERFACE__V1__COMPLETION__INIT;
    done.by = YOKE__INTERFACE__V1__COMPLETION__BY__BY_CORE;
    Yoke__Interface__V1__CoreFrame f = YOKE__INTERFACE__V1__CORE_FRAME__INIT;
    f.call = (char *)call;
    f.carries_case = YOKE__INTERFACE__V1__CORE_FRAME__CARRIES_COMPLETION;
    f.completion = &done;
    core_send(c, &f);
}

static void say_snapshot(core_conn *c, const char *call) {
    Yoke__Interface__V1__Snapshot snapshot = YOKE__INTERFACE__V1__SNAPSHOT__INIT;
    Yoke__Interface__V1__Subscribed subscribed = YOKE__INTERFACE__V1__SUBSCRIBED__INIT;
    subscribed.carries_case = YOKE__INTERFACE__V1__SUBSCRIBED__CARRIES_SNAPSHOT;
    subscribed.snapshot = &snapshot;
    Yoke__Interface__V1__Response r = YOKE__INTERFACE__V1__RESPONSE__INIT;
    r.answer_case = YOKE__INTERFACE__V1__RESPONSE__ANSWER_SUBSCRIBE;
    r.subscribe = &subscribed;
    say_answer(c, call, &r);
}

/* A record of one unit, declared by its identity. */
static Yoke__Interface__V1__Record *unit_record(const char *identity) {
    Yoke__Interface__V1__Record *r = malloc(sizeof *r);
    yoke__interface__v1__record__init(r);
    Yoke__Interface__V1__UnitRecord *u = malloc(sizeof *u);
    yoke__interface__v1__unit_record__init(u);
    Yoke__Interface__V1__UnitRecord__Declared *d = malloc(sizeof *d);
    yoke__interface__v1__unit_record__declared__init(d);
    d->identity = strdup(identity);
    d->kind = strdup("plugin");
    u->declared = d;
    r->subject_case = YOKE__INTERFACE__V1__RECORD__SUBJECT_UNIT;
    r->unit = u;
    return r;
}

static const Yoke__Interface__V1__Request *request_of(const Yoke__Interface__V1__ClientFrame *f) {
    return f->carries_case == YOKE__INTERFACE__V1__CLIENT_FRAME__CARRIES_REQUEST ? f->request : NULL;
}

/* A Core that opens at the version and subscription it is handed, and reads whatever comes. */
typedef struct opens {
    const char *subscription;
    unsigned version;
    size_t records;
} opens;

static void script_opens(core_conn *c, void *arg) {
    const opens *o = arg;
    Yoke__Interface__V1__Record *records[1];
    for (size_t i = 0; i < o->records; i++) {
        records[i] = unit_record("acquire");
    }
    core_send(c, core_opening(o->subscription, o->version, records, o->records));
    const Yoke__Interface__V1__ClientFrame *f;
    while (core_next(c, &f, -1) == CORE_FRAME) {
    }
}

static void script_refuses_the_channel(core_conn *c, void *arg) {
    (void)arg;
    core_refuse(c, "channel.in_use", "the channel has its client");
}

static bool until_ended(core *core_, int ms) {
    for (int waited = 0; waited < ms; waited += 10) {
        if (core_ends(core_) > 0) {
            return true;
        }
        check_sleep(10);
    }
    return false;
}

/* The error's code, or "" when it is not a refusal. */
static const char *code_of(const yoke_error *err) { return err->code ? err->code : ""; }

/* A getenv over a few names and their values. */
static const char *lookup(const char *name, void *context) {
    const char *const *pairs = context;
    for (size_t i = 0; pairs[i] != NULL; i += 2) {
        if (strcmp(pairs[i], name) == 0) {
            return pairs[i + 1];
        }
    }
    return NULL;
}

// std: yoke-sdk-c:the-interface-library.01
static void test_a_channels_address_is_computed_or_handed_to_a_managed_interface(void) {
    char *service = NULL, *application = NULL, *managed = NULL, *never = NULL;
    yoke_error err = {0};
    const char *const set[] = {"XDG_RUNTIME_DIR", "/run/user/1000", "YOKE_SOCKET",
                               "/run/yoke/interfaces/panel.sock", NULL};
    const char *const none[] = {NULL};
    REQUIRE(yoke_interface_service(&service, "/run/yoke", "panel", &err) == 0 &&
                strcmp(service, "/run/yoke/interfaces/panel.sock") == 0,
            "the service address is %s", service ? service : "none");
    REQUIRE(yoke_interface_application(&application, "bench-a", "panel", lookup, (void *)set, &err) == 0 &&
                strcmp(application, "/run/user/1000/yoke/bench-a/interfaces/panel.sock") == 0,
            "the application address is %s", application ? application : "none");
    REQUIRE(yoke_interface_managed(&managed, lookup, (void *)set, &err) == 0 &&
                strcmp(managed, "/run/yoke/interfaces/panel.sock") == 0,
            "the managed address is %s", managed ? managed : "none");
    REQUIRE(yoke_interface_managed(&never, lookup, (void *)none, &err) != 0 && err.message != NULL &&
                strstr(err.message, "YOKE_SOCKET") != NULL,
            "a managed interface handed no address was not refused naming it");
    char *bad = NULL;
    REQUIRE(yoke_interface_service(&bad, "/run/yoke", "a/b", &err) != 0,
            "a channel name with a slash was taken");
    REQUIRE(yoke_interface_service(&bad, "/run/yoke", "..", &err) != 0, "the channel name .. was taken");
    REQUIRE(yoke_interface_application(&bad, "a/b", "panel", lookup, (void *)set, &err) != 0,
            "an instance name with a slash was taken");
done:
    free(service);
    free(application);
    free(managed);
    free(never);
    yoke_error_clear(&err);
}

// std: yoke-sdk-c:the-interface-library.02
static void test_attaching_surfaces_the_opening_and_a_version_it_does_not_speak_is_refused(void) {
    opens good = {.subscription = "standing", .version = 1, .records = 1};
    opens other = {.subscription = "standing", .version = 2};
    core *first = core_start(check_dir(), script_opens, &good);
    yoke_error err = {0};
    yoke_attachment *a = yoke_attach(core_socket(first), NULL, &err);
    REQUIRE(a != NULL, "the first attachment failed: %s", err.message ? err.message : "");
    REQUIRE(yoke_interface_version(a) == 1 && yoke_standing(a) != NULL &&
                strcmp(yoke_subscription_id(yoke_standing(a)), "standing") == 0,
            "the opening did not carry version 1 and the standing subscription");
    REQUIRE(yoke_picture(a)->n_records == 1 && yoke_picture(a)->records[0]->unit != NULL &&
                strcmp(yoke_picture(a)->records[0]->unit->declared->identity, "acquire") == 0,
            "the picture did not hold the unit's record");
    yoke_detach(a);
    a = NULL;
    core_stop(first);

    static char dir2[300];
    snprintf(dir2, sizeof dir2, "%s/two", check_dir());
    mkdir(dir2, 0700);
    core *second = core_start(dir2, script_opens, &other);
    yoke_attachment *b = yoke_attach(core_socket(second), NULL, &err);
    REQUIRE(b == NULL && yoke_is_refusal(&err, "compat.unsupported") && strstr(err.message, "1") != NULL &&
                strstr(err.message, "2") != NULL,
            "version 2 was not refused with both versions: %s", err.message ? err.message : "");
    REQUIRE(until_ended(second, 5000), "the connection was not let go");
    core_stop(second);
    yoke_error_clear(&err);

    static char dir3[300];
    snprintf(dir3, sizeof dir3, "%s/three", check_dir());
    mkdir(dir3, 0700);
    core *third = core_start(dir3, script_refuses_the_channel, NULL);
    yoke_attachment *c = yoke_attach(core_socket(third), NULL, &err);
    REQUIRE(c == NULL && yoke_is_refusal(&err, "channel.in_use"), "the refused attachment answered %s: %s",
            code_of(&err), err.message ? err.message : "");
    core_stop(third);
done:
    if (a != NULL) {
        yoke_detach(a);
    }
    yoke_error_clear(&err);
}

/* ---- case 03: every operation, and two calls at once ---- */

static void script_every_operation(core_conn *c, void *arg) {
    (void)arg;
    core_send(c, core_opening("standing", 1, NULL, 0));
    char slow[32] = "";
    const Yoke__Interface__V1__ClientFrame *f;
    while (core_next(c, &f, -1) == CORE_FRAME) {
        const Yoke__Interface__V1__Request *r = request_of(f);
        if (r == NULL) {
            continue;
        }
        Yoke__Interface__V1__Response resp = YOKE__INTERFACE__V1__RESPONSE__INIT;
        Yoke__Interface__V1__Authenticated authenticated = YOKE__INTERFACE__V1__AUTHENTICATED__INIT;
        Yoke__Interface__V1__Records records = YOKE__INTERFACE__V1__RECORDS__INIT;
        Yoke__Interface__V1__Confirmed confirmed = YOKE__INTERFACE__V1__CONFIRMED__INIT;
        Yoke__Interface__V1__Acknowledged ack = YOKE__INTERFACE__V1__ACKNOWLEDGED__INIT;
        Yoke__Interface__V1__Answered answered = YOKE__INTERFACE__V1__ANSWERED__INIT;
        Yoke__Interface__V1__Delivering delivering = YOKE__INTERFACE__V1__DELIVERING__INIT;
        Yoke__Interface__V1__Released released = YOKE__INTERFACE__V1__RELEASED__INIT;
        Yoke__Interface__V1__Reclaimed reclaimed = YOKE__INTERFACE__V1__RECLAIMED__INIT;
        bool reply = true;
        switch (r->operation_case) {
        case YOKE__INTERFACE__V1__REQUEST__OPERATION_AUTHENTICATE:
            authenticated.account = (char *)"alice";
            authenticated.token = (char *)"t-1";
            resp.answer_case = YOKE__INTERFACE__V1__RESPONSE__ANSWER_AUTHENTICATE;
            resp.authenticate = &authenticated;
            break;
        case YOKE__INTERFACE__V1__REQUEST__OPERATION_READ:
            resp.answer_case = YOKE__INTERFACE__V1__RESPONSE__ANSWER_READ;
            resp.read = &records;
            break;
        case YOKE__INTERFACE__V1__REQUEST__OPERATION_SUBSCRIBE:
            say_snapshot(c, f->call);
            reply = false;
            break;
        case YOKE__INTERFACE__V1__REQUEST__OPERATION_CONFIRM:
            resp.answer_case = YOKE__INTERFACE__V1__RESPONSE__ANSWER_CONFIRM;
            resp.confirm = &confirmed;
            break;
        case YOKE__INTERFACE__V1__REQUEST__OPERATION_COMMAND:
            ack.outcome = YOKE__INTERFACE__V1__ACKNOWLEDGED__OUTCOME__OUTCOME_DONE;
            resp.answer_case = YOKE__INTERFACE__V1__RESPONSE__ANSWER_COMMAND;
            resp.command = &ack;
            say_answer(c, f->call, &resp);
            reply = false;
            if (strcmp(r->command->unit, "fast") == 0 && slow[0] != '\0') {
                /* The question's answer comes after the command's has been taken. */
                check_sleep(150);
                answered.payload.data = (uint8_t *)"slow";
                answered.payload.len = 4;
                Yoke__Interface__V1__Response late = YOKE__INTERFACE__V1__RESPONSE__INIT;
                late.answer_case = YOKE__INTERFACE__V1__RESPONSE__ANSWER_QUERY;
                late.query = &answered;
                say_answer(c, slow, &late);
                slow[0] = '\0';
            }
            break;
        case YOKE__INTERFACE__V1__REQUEST__OPERATION_QUERY:
            if (strcmp(r->query->unit, "slow") == 0) {
                snprintf(slow, sizeof slow, "%s", f->call);
                reply = false;
                break;
            }
            answered.payload.data = (uint8_t *)"answer";
            answered.payload.len = 6;
            resp.answer_case = YOKE__INTERFACE__V1__RESPONSE__ANSWER_QUERY;
            resp.query = &answered;
            break;
        case YOKE__INTERFACE__V1__REQUEST__OPERATION_STREAM_START:
        case YOKE__INTERFACE__V1__REQUEST__OPERATION_STREAM_STOP:
            ack.outcome = YOKE__INTERFACE__V1__ACKNOWLEDGED__OUTCOME__OUTCOME_ACCEPTED;
            if (r->operation_case == YOKE__INTERFACE__V1__REQUEST__OPERATION_STREAM_START) {
                resp.answer_case = YOKE__INTERFACE__V1__RESPONSE__ANSWER_STREAM_START;
                resp.stream_start = &ack;
            } else {
                resp.answer_case = YOKE__INTERFACE__V1__RESPONSE__ANSWER_STREAM_STOP;
                resp.stream_stop = &ack;
            }
            break;
        case YOKE__INTERFACE__V1__REQUEST__OPERATION_STREAM_SUBSCRIBE:
            delivering.delivery = (char *)"00000001";
            delivering.arrives_case = YOKE__INTERFACE__V1__DELIVERING__ARRIVES_CONNECTION;
            delivering.connection = 1;
            delivering.flowing = 1;
            resp.answer_case = YOKE__INTERFACE__V1__RESPONSE__ANSWER_STREAM_SUBSCRIBE;
            resp.stream_subscribe = &delivering;
            break;
        case YOKE__INTERFACE__V1__REQUEST__OPERATION_STREAM_UNSUBSCRIBE:
            resp.answer_case = YOKE__INTERFACE__V1__RESPONSE__ANSWER_STREAM_UNSUBSCRIBE;
            resp.stream_unsubscribe = &released;
            break;
        case YOKE__INTERFACE__V1__REQUEST__OPERATION_RECLAIM:
            reclaimed.changed = 1;
            resp.answer_case = YOKE__INTERFACE__V1__RESPONSE__ANSWER_RECLAIM;
            resp.reclaim = &reclaimed;
            break;
        default:
            reply = false;
            break;
        }
        if (reply) {
            say_answer(c, f->call, &resp);
        }
    }
}

typedef struct timing {
    yoke_attachment *a;
    long long done_at;
    yoke_error err;
    int rc;
} timing;

static void *asks_slow(void *arg) {
    timing *t = arg;
    Yoke__Interface__V1__Response *answer = NULL;
    t->rc = yoke_ask(t->a, "slow", "status", "", 0, &answer, &t->err);
    t->done_at = check_ms();
    yoke_response_free(answer);
    return NULL;
}

static const char *const operations[] = {"authenticate",
                                         "command",
                                         "confirm",
                                         "query",
                                         "read",
                                         "reclaim",
                                         "stream.start",
                                         "stream.stop",
                                         "stream.subscribe",
                                         "stream.unsubscribe",
                                         "subscribe"};

static const char *operation_name(const Yoke__Interface__V1__Request *r) {
    switch (r->operation_case) {
    case YOKE__INTERFACE__V1__REQUEST__OPERATION_AUTHENTICATE:
        return "authenticate";
    case YOKE__INTERFACE__V1__REQUEST__OPERATION_READ:
        return "read";
    case YOKE__INTERFACE__V1__REQUEST__OPERATION_SUBSCRIBE:
        return "subscribe";
    case YOKE__INTERFACE__V1__REQUEST__OPERATION_CONFIRM:
        return "confirm";
    case YOKE__INTERFACE__V1__REQUEST__OPERATION_COMMAND:
        return "command";
    case YOKE__INTERFACE__V1__REQUEST__OPERATION_QUERY:
        return "query";
    case YOKE__INTERFACE__V1__REQUEST__OPERATION_STREAM_START:
        return "stream.start";
    case YOKE__INTERFACE__V1__REQUEST__OPERATION_STREAM_STOP:
        return "stream.stop";
    case YOKE__INTERFACE__V1__REQUEST__OPERATION_STREAM_SUBSCRIBE:
        return "stream.subscribe";
    case YOKE__INTERFACE__V1__REQUEST__OPERATION_STREAM_UNSUBSCRIBE:
        return "stream.unsubscribe";
    case YOKE__INTERFACE__V1__REQUEST__OPERATION_RECLAIM:
        return "reclaim";
    default:
        return "";
    }
}

// std: yoke-sdk-c:the-interface-library.03
static void test_one_function_per_operation_each_stating_the_version_on_one_connection(void) {
    core *k = core_start(check_dir(), script_every_operation, NULL);
    yoke_error err = {0};
    yoke_attachment *a = yoke_attach(core_socket(k), NULL, &err);
    Yoke__Interface__V1__Response *r = NULL;
    REQUIRE(a != NULL, "the attachment failed: %s", err.message ? err.message : "");
    REQUIRE(yoke_authenticate(a, "alice", "hunter2x", &r, &err) == 0 &&
                strcmp(r->authenticate->account, "alice") == 0,
            "authenticate");
    yoke_response_free(r);
    REQUIRE(yoke_read(a, "unit", "", &r, &err) == 0, "read");
    yoke_response_free(r);
    yoke_subscription *s = yoke_subscribe(a, NULL, &err);
    Yoke__Interface__V1__Subscribed *d = NULL;
    REQUIRE(s != NULL && yoke_subscription_next(s, &d, 5000, &err) == YOKE_WAIT_GOT &&
                d->carries_case == YOKE__INTERFACE__V1__SUBSCRIBED__CARRIES_SNAPSHOT,
            "subscribe");
    yoke_subscribed_free(d);
    yoke_subscription_close(s);
    REQUIRE(yoke_confirm(a, "standing", 1, &r, &err) == 0, "confirm");
    yoke_response_free(r);
    REQUIRE(yoke_command(a, "acquire", "calibrate", "x", 1, &r, &err) == 0, "command");
    yoke_response_free(r);
    REQUIRE(yoke_ask(a, "acquire", "status", "", 0, &r, &err) == 0 && r->query->payload.len == 6, "query");
    yoke_response_free(r);
    REQUIRE(yoke_start_stream(a, "acquire", "spectra", &r, &err) == 0, "stream.start");
    yoke_response_free(r);
    REQUIRE(yoke_stop_stream(a, "acquire", "spectra", &r, &err) == 0, "stream.stop");
    yoke_response_free(r);
    yoke_delivery *delivery = yoke_subscribe_stream(a, "acquire", "spectra", &err);
    REQUIRE(delivery != NULL && yoke_delivery_flowing(delivery), "stream.subscribe");
    REQUIRE(yoke_delivery_release(delivery, &err) == 0, "stream.unsubscribe");
    REQUIRE(yoke_reclaim(a, &r, &err) == 0 && r->reclaim->changed, "reclaim");
    yoke_response_free(r);

    /* A question that waits for the Core, and a command that does not: each returns its own answer. */
    timing slow = {.a = a};
    pthread_t t;
    pthread_create(&t, NULL, asks_slow, &slow);
    check_sleep(100);
    REQUIRE(yoke_command(a, "fast", "calibrate", "", 0, &r, &err) == 0 &&
                r->command->outcome == YOKE__INTERFACE__V1__ACKNOWLEDGED__OUTCOME__OUTCOME_DONE,
            "the command's own answer did not come");
    long long command_at = check_ms();
    yoke_response_free(r);
    pthread_join(t, NULL);
    REQUIRE(slow.rc == 0 && slow.done_at >= command_at,
            "the question did not come after the command, with its own answer");
    yoke_error_clear(&slow.err);
    yoke_detach(a);
    a = NULL;

    REQUIRE(core_attachments(k) == 1, "%d attachments", core_attachments(k));
    for (size_t i = 0; i < sizeof operations / sizeof *operations; i++) {
        int seen = 0;
        for (size_t n = 0; n < core_frames(k); n++) {
            const Yoke__Interface__V1__Request *q = request_of(core_frame(k, n, NULL));
            if (q != NULL && strcmp(operation_name(q), operations[i]) == 0) {
                seen++;
                REQUIRE(q->version == 1, "%s states the version %u", operations[i], q->version);
            }
        }
        REQUIRE(seen > 0, "%s was never sent", operations[i]);
    }
done:
    if (a != NULL) {
        yoke_detach(a);
    }
    yoke_error_clear(&err);
    core_stop(k);
}

/* ---- case 04: refusals ---- */

static void script_refuses_what_it_is_asked(core_conn *c, void *arg) {
    (void)arg;
    core_send(c, core_opening("standing", 1, NULL, 0));
    const Yoke__Interface__V1__ClientFrame *f;
    while (core_next(c, &f, -1) == CORE_FRAME) {
        const Yoke__Interface__V1__Request *r = request_of(f);
        if (r == NULL) {
            continue;
        }
        Yoke__Interface__V1__Refusal refusal = YOKE__INTERFACE__V1__REFUSAL__INIT;
        Yoke__Interface__V1__Subject subject = YOKE__INTERFACE__V1__SUBJECT__INIT;
        Yoke__Interface__V1__Suspension suspension = YOKE__INTERFACE__V1__SUSPENSION__INIT;
        if (r->operation_case == YOKE__INTERFACE__V1__REQUEST__OPERATION_READ) {
            refusal.code = (char *)"subject.unknown";
            refusal.message = (char *)"no such unit";
            subject.kind = (char *)"unit";
            subject.identity = (char *)"nobody";
            refusal.detail_case = YOKE__INTERFACE__V1__REFUSAL__DETAIL_SUBJECT;
            refusal.subject = &subject;
        } else {
            refusal.code = (char *)"channel.suspended";
            refusal.message = (char *)"suspended";
            suspension.grade = (char *)"read-only";
            suspension.by = (char *)"service";
            refusal.detail_case = YOKE__INTERFACE__V1__REFUSAL__DETAIL_SUSPENSION;
            refusal.suspension = &suspension;
        }
        say_refusal(c, f->call, &refusal);
    }
}

// std: yoke-sdk-c:the-interface-library.04
static void test_a_refusal_is_an_error_with_its_code_and_its_detail(void) {
    core *k = core_start(check_dir(), script_refuses_what_it_is_asked, NULL);
    yoke_error err = {0};
    yoke_attachment *a = yoke_attach(core_socket(k), NULL, &err);
    Yoke__Interface__V1__Response *r = NULL;
    REQUIRE(a != NULL, "the attachment failed");
    REQUIRE(yoke_read(a, "unit", "nobody", &r, &err) != 0 && yoke_is_refusal(&err, "subject.unknown") &&
                err.subject.kind != NULL && strcmp(err.subject.kind, "unit") == 0 &&
                strcmp(err.subject.identity, "nobody") == 0,
            "the read's refusal was %s", code_of(&err));
    yoke_error_clear(&err);
    REQUIRE(yoke_command(a, "acquire", "calibrate", "", 0, &r, &err) != 0 &&
                yoke_is_refusal(&err, "channel.suspended") && err.suspension.grade != NULL &&
                strcmp(err.suspension.grade, "read-only") == 0 && strcmp(err.suspension.by, "service") == 0,
            "the command's refusal was %s", code_of(&err));
done:
    yoke_error_clear(&err);
    if (a != NULL) {
        yoke_detach(a);
    }
    core_stop(k);
}

/* ---- case 05: subscriptions ---- */

static void script_subscribes(core_conn *c, void *arg) {
    (void)arg;
    core_send(c, core_opening("standing", 1, NULL, 0));
    for (uint64_t seq = 1; seq <= 3; seq++) {
        say_event(c, "standing", seq);
    }
    Yoke__Interface__V1__Record *records[1] = {unit_record("acquire")};
    Yoke__Interface__V1__Snapshot snapshot = YOKE__INTERFACE__V1__SNAPSHOT__INIT;
    snapshot.at = 9;
    snapshot.records = records;
    snapshot.n_records = 1;
    Yoke__Interface__V1__Subscribed overflow = YOKE__INTERFACE__V1__SUBSCRIBED__INIT;
    overflow.carries_case = YOKE__INTERFACE__V1__SUBSCRIBED__CARRIES_OVERFLOW;
    overflow.overflow = &snapshot;
    Yoke__Interface__V1__Response r = YOKE__INTERFACE__V1__RESPONSE__INIT;
    r.answer_case = YOKE__INTERFACE__V1__RESPONSE__ANSWER_SUBSCRIBE;
    r.subscribe = &overflow;
    say_answer(c, "standing", &r);
    const Yoke__Interface__V1__ClientFrame *f;
    while (core_next(c, &f, -1) == CORE_FRAME) {
        const Yoke__Interface__V1__Request *req = request_of(f);
        if (req != NULL && req->operation_case == YOKE__INTERFACE__V1__REQUEST__OPERATION_SUBSCRIBE) {
            say_snapshot(c, f->call);
            for (uint64_t seq = 10; seq <= 11; seq++) {
                say_event(c, f->call, seq);
            }
        } else if (f->carries_case == YOKE__INTERFACE__V1__CLIENT_FRAME__CARRIES_CANCEL) {
            /* The Core ends the attachment once the subscription is cancelled. */
            core_finish(c);
            return;
        }
    }
}

static bool cancelled(core *k, const char *call) {
    for (size_t n = 0; n < core_frames(k); n++) {
        const Yoke__Interface__V1__ClientFrame *f = core_frame(k, n, NULL);
        if (strcmp(f->call, call) == 0 &&
            f->carries_case == YOKE__INTERFACE__V1__CLIENT_FRAME__CARRIES_CANCEL) {
            return true;
        }
    }
    return false;
}

// std: yoke-sdk-c:the-interface-library.05
static void test_a_subscription_is_delivered_in_order_until_ended_and_nothing_reconnects(void) {
    core *k = core_start(check_dir(), script_subscribes, NULL);
    yoke_error err = {0};
    yoke_attachment *a = yoke_attach(core_socket(k), NULL, &err);
    Yoke__Interface__V1__Subscribed *d = NULL;
    Yoke__Interface__V1__Response *r = NULL;
    REQUIRE(a != NULL, "the attachment failed");
    yoke_subscription *standing = yoke_standing(a);
    for (uint64_t seq = 1; seq <= 3; seq++) {
        REQUIRE(yoke_subscription_next(standing, &d, 5000, &err) == YOKE_WAIT_GOT &&
                    d->carries_case == YOKE__INTERFACE__V1__SUBSCRIBED__CARRIES_EVENT && d->event->seq == seq,
                "standing event %llu", (unsigned long long)seq);
        yoke_subscribed_free(d);
    }
    REQUIRE(yoke_subscription_next(standing, &d, 5000, &err) == YOKE_WAIT_GOT &&
                d->carries_case == YOKE__INTERFACE__V1__SUBSCRIBED__CARRIES_OVERFLOW && d->overflow->at == 9,
            "the overflow");
    yoke_subscribed_free(d);
    yoke_subscription *sub = yoke_subscribe(a, NULL, &err);
    REQUIRE(sub != NULL && yoke_subscription_next(sub, &d, 5000, &err) == YOKE_WAIT_GOT &&
                d->carries_case == YOKE__INTERFACE__V1__SUBSCRIBED__CARRIES_SNAPSHOT,
            "the snapshot");
    yoke_subscribed_free(d);
    for (uint64_t seq = 10; seq <= 11; seq++) {
        REQUIRE(yoke_subscription_next(sub, &d, 5000, &err) == YOKE_WAIT_GOT &&
                    d->carries_case == YOKE__INTERFACE__V1__SUBSCRIBED__CARRIES_EVENT && d->event->seq == seq,
                "event %llu", (unsigned long long)seq);
        yoke_subscribed_free(d);
    }
    char call[32];
    snprintf(call, sizeof call, "%s", yoke_subscription_id(sub));
    yoke_subscription_close(sub);
    bool saw = false;
    for (int i = 0; i < 500 && !saw; i++) {
        saw = cancelled(k, call);
        check_sleep(10);
    }
    REQUIRE(saw, "ending the subscription did not cancel it on the wire");
    bool ended = false;
    for (int i = 0; i < 500 && !ended; i++) {
        yoke_error_clear(&err);
        if (yoke_read(a, "unit", "", &r, &err) != 0) {
            ended = err.message != NULL && strstr(err.message, "ended") != NULL;
        } else {
            yoke_response_free(r);
        }
        check_sleep(20);
    }
    REQUIRE(ended, "a call after the end did not answer that it ended");
    REQUIRE(core_attachments(k) == 1, "something reconnected: %d attachments", core_attachments(k));
done:
    yoke_error_clear(&err);
    if (a != NULL) {
        yoke_detach(a);
    }
    core_stop(k);
}

// std: yoke-sdk-c:the-interface-library.06
static void test_confirmation_is_sent_only_when_the_author_confirms(void) {
    core *k = core_start(check_dir(), script_every_operation, NULL);
    yoke_error err = {0};
    yoke_attachment *a = yoke_attach(core_socket(k), NULL, &err);
    Yoke__Interface__V1__Response *r = NULL;
    REQUIRE(a != NULL, "the attachment failed");
    check_sleep(400);
    REQUIRE(core_frames(k) == 0, "the library sent %zu frames unasked", core_frames(k));
    REQUIRE(yoke_confirm(a, "standing", 7, &r, &err) == 0, "the confirmation failed");
    yoke_response_free(r);
    REQUIRE(core_frames(k) == 1, "%zu frames were sent", core_frames(k));
    const Yoke__Interface__V1__Request *q = request_of(core_frame(k, 0, NULL));
    REQUIRE(q != NULL && q->operation_case == YOKE__INTERFACE__V1__REQUEST__OPERATION_CONFIRM &&
                strcmp(q->confirm->subscription, "standing") == 0 && q->confirm->sequence == 7,
            "the one request was not the confirmation of `standing` at 7");
done:
    yoke_error_clear(&err);
    if (a != NULL) {
        yoke_detach(a);
    }
    core_stop(k);
}

/* ---- case 07: deliveries ---- */

typedef struct delivery_script {
    char socket[300];
} delivery_script;

static void script_delivers(core_conn *c, void *arg) {
    const delivery_script *d = arg;
    core_send(c, core_opening("standing", 1, NULL, 0));
    int asked = 0;
    const Yoke__Interface__V1__ClientFrame *f;
    while (core_next(c, &f, -1) == CORE_FRAME) {
        const Yoke__Interface__V1__Request *r = request_of(f);
        if (r == NULL) {
            continue;
        }
        if (r->operation_case == YOKE__INTERFACE__V1__REQUEST__OPERATION_STREAM_SUBSCRIBE) {
            asked++;
            Yoke__Interface__V1__Delivering delivering = YOKE__INTERFACE__V1__DELIVERING__INIT;
            Yoke__Interface__V1__Response resp = YOKE__INTERFACE__V1__RESPONSE__INIT;
            resp.answer_case = YOKE__INTERFACE__V1__RESPONSE__ANSWER_STREAM_SUBSCRIBE;
            resp.stream_subscribe = &delivering;
            if (asked == 1) {
                delivering.delivery = (char *)"00000001";
                delivering.arrives_case = YOKE__INTERFACE__V1__DELIVERING__ARRIVES_SOCKET;
                delivering.socket = (char *)d->socket;
                delivering.flowing = 1;
            } else {
                delivering.delivery = (char *)"00000002";
                delivering.arrives_case = YOKE__INTERFACE__V1__DELIVERING__ARRIVES_CONNECTION;
                delivering.connection = 1;
            }
            say_answer(c, f->call, &resp);
            if (asked == 2) {
                for (uint64_t n = 1; n <= 2; n++) {
                    char payload[16];
                    snprintf(payload, sizeof payload, "frame %llu", (unsigned long long)n);
                    Yoke__Interface__V1__StreamDelivery sd = YOKE__INTERFACE__V1__STREAM_DELIVERY__INIT;
                    sd.delivery = (char *)"00000002";
                    sd.sequence = n;
                    sd.sent_at = 2000 + n;
                    sd.payload.data = (uint8_t *)payload;
                    sd.payload.len = strlen(payload);
                    Yoke__Interface__V1__CoreFrame frame = YOKE__INTERFACE__V1__CORE_FRAME__INIT;
                    frame.call = (char *)"00000002";
                    frame.carries_case = YOKE__INTERFACE__V1__CORE_FRAME__CARRIES_DELIVERY;
                    frame.delivery = &sd;
                    core_send(c, &frame);
                }
                say_completion(c, "00000002");
            }
        } else if (r->operation_case == YOKE__INTERFACE__V1__REQUEST__OPERATION_STREAM_UNSUBSCRIBE) {
            Yoke__Interface__V1__Released released = YOKE__INTERFACE__V1__RELEASED__INIT;
            Yoke__Interface__V1__Response resp = YOKE__INTERFACE__V1__RESPONSE__INIT;
            resp.answer_case = YOKE__INTERFACE__V1__RESPONSE__ANSWER_STREAM_UNSUBSCRIBE;
            resp.stream_unsubscribe = &released;
            say_answer(c, f->call, &resp);
        }
    }
}

typedef struct socket_sender {
    int listener;
} socket_sender;

static void *send_two_frames(void *arg) {
    socket_sender *s = arg;
    int conn = accept(s->listener, NULL, NULL);
    for (uint64_t n = 1; n <= 2 && conn >= 0; n++) {
        unsigned char packet[64];
        char text[16];
        int len = snprintf(text, sizeof text, "frame %llu", (unsigned long long)n);
        uint64_t sequence = n, clock = 1000 + n;
        for (int i = 0; i < 8; i++) {
            packet[i] = (unsigned char)(sequence >> (8 * i));
            packet[8 + i] = (unsigned char)(clock >> (8 * i));
        }
        memcpy(packet + 16, text, (size_t)len);
        if (send(conn, packet, 16 + (size_t)len, 0) < 0) {
            break;
        }
    }
    if (conn >= 0) {
        close(conn);
    }
    return NULL;
}

// std: yoke-sdk-c:the-interface-library.07
static void test_a_streams_delivery_is_read_where_its_answer_says_it_arrives(void) {
    core *k = NULL;
    yoke_attachment *a = NULL;
    pthread_t thread;
    bool sending = false;
    yoke_error err = {0};
    delivery_script script = {0};
    snprintf(script.socket, sizeof script.socket, "%s/delivery.sock", check_dir());
    int listener = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    struct sockaddr_un addr = {.sun_family = AF_UNIX};
    strcpy(addr.sun_path, script.socket);
    REQUIRE(listener >= 0 && bind(listener, (struct sockaddr *)&addr, sizeof addr) == 0 &&
                listen(listener, 1) == 0,
            "the case cannot listen at its socket");
    socket_sender sender = {listener};
    pthread_create(&thread, NULL, send_two_frames, &sender);
    sending = true;

    k = core_start(check_dir(), script_delivers, &script);
    a = yoke_attach(core_socket(k), NULL, &err);
    REQUIRE(a != NULL, "the attachment failed");
    yoke_delivery *by_socket = yoke_subscribe_stream(a, "acquire", "spectra", &err);
    REQUIRE(by_socket != NULL && yoke_delivery_flowing(by_socket), "the delivery by socket did not flow");
    for (uint64_t n = 1; n <= 2; n++) {
        yoke_frame frame;
        char want[16];
        snprintf(want, sizeof want, "frame %llu", (unsigned long long)n);
        REQUIRE(yoke_delivery_next(by_socket, &frame, 5000, &err) == YOKE_WAIT_GOT && frame.sequence == n &&
                    frame.sent_at == 1000 + n && frame.payload_len == strlen(want) &&
                    memcmp(frame.payload, want, frame.payload_len) == 0,
                "frame %llu on the socket", (unsigned long long)n);
        yoke_frame_clear(&frame);
    }
    yoke_frame frame;
    REQUIRE(yoke_delivery_next(by_socket, &frame, 5000, &err) == YOKE_WAIT_COMPLETE, "the socket's end");
    yoke_delivery_release(by_socket, NULL);
    yoke_delivery *by_connection = yoke_subscribe_stream(a, "acquire", "spectra", &err);
    REQUIRE(by_connection != NULL && !yoke_delivery_flowing(by_connection), "the delivery on the connection");
    for (uint64_t n = 1; n <= 2; n++) {
        char want[16];
        snprintf(want, sizeof want, "frame %llu", (unsigned long long)n);
        REQUIRE(yoke_delivery_next(by_connection, &frame, 5000, &err) == YOKE_WAIT_GOT &&
                    frame.sequence == n && frame.sent_at == 2000 + n && frame.payload_len == strlen(want) &&
                    memcmp(frame.payload, want, frame.payload_len) == 0,
                "frame %llu on the connection", (unsigned long long)n);
        yoke_frame_clear(&frame);
    }
    REQUIRE(yoke_delivery_next(by_connection, &frame, 5000, &err) == YOKE_WAIT_COMPLETE,
            "the delivery's end");
    REQUIRE(yoke_delivery_release(by_connection, &err) == 0, "the release failed");
    int released = 0;
    for (size_t n = 0; n < core_frames(k); n++) {
        const Yoke__Interface__V1__Request *q = request_of(core_frame(k, n, NULL));
        if (q != NULL && q->operation_case == YOKE__INTERFACE__V1__REQUEST__OPERATION_STREAM_UNSUBSCRIBE) {
            released += strcmp(q->stream_unsubscribe->delivery, "00000002") == 0;
        }
    }
    REQUIRE(released == 1, "stream.unsubscribe naming the delivery was sent %d times", released);
done:
    yoke_error_clear(&err);
    if (a != NULL) {
        yoke_detach(a);
    }
    if (k != NULL) {
        core_stop(k);
    }
    if (sending) {
        pthread_join(thread, NULL);
    }
}

static void script_closes(core_conn *c, void *arg) {
    (void)arg;
    core_send(c, core_opening("standing", 1, NULL, 0));
    const Yoke__Interface__V1__ClientFrame *f;
    int got;
    while ((got = core_next(c, &f, -1)) == CORE_FRAME) {
        const Yoke__Interface__V1__Request *r = request_of(f);
        if (r != NULL && r->operation_case == YOKE__INTERFACE__V1__REQUEST__OPERATION_READ) {
            Yoke__Interface__V1__Records records = YOKE__INTERFACE__V1__RECORDS__INIT;
            Yoke__Interface__V1__Response resp = YOKE__INTERFACE__V1__RESPONSE__INIT;
            resp.answer_case = YOKE__INTERFACE__V1__RESPONSE__ANSWER_READ;
            resp.read = &records;
            say_answer(c, f->call, &resp);
        }
    }
    /* The Core ends the stream once the client's requests have ended. */
    if (got == CORE_CLOSED) {
        core_finish(c);
    }
}

// std: yoke-sdk-c:the-interface-library.08
static void test_closing_an_attachment_ends_it_in_order(void) {
    core *k = core_start(check_dir(), script_closes, NULL);
    yoke_error err = {0};
    yoke_attachment *a = yoke_attach(core_socket(k), NULL, &err);
    Yoke__Interface__V1__Response *r = NULL;
    REQUIRE(a != NULL, "the attachment failed");
    REQUIRE(yoke_read(a, "unit", "", &r, &err) == 0, "the call failed");
    yoke_response_free(r);
    long long before = check_ms();
    yoke_detach(a);
    a = NULL;
    REQUIRE(check_ms() - before < 900, "the close waited out its grace: %lld ms", check_ms() - before);
    REQUIRE(core_ends(k) == 1 && strcmp(core_end(k, 0), "closed") == 0, "the Core read %s",
            core_ends(k) ? core_end(k, 0) : "nothing");
done:
    yoke_error_clear(&err);
    if (a != NULL) {
        yoke_detach(a);
    }
    core_stop(k);
}

static char *code_of_file(const char *path) {
    char *text = check_read(path);
    if (text == NULL) {
        return NULL;
    }
    char *out = text;
    for (char *in = text; *in != '\0';) {
        if (in[0] == '/' && in[1] == '*') {
            char *end = strstr(in + 2, "*/");
            in = end ? end + 2 : in + strlen(in);
        } else if (in[0] == '/' && in[1] == '/') {
            while (*in != '\0' && *in != '\n') {
                in++;
            }
        } else {
            *out++ = *in++;
        }
    }
    *out = '\0';
    return text;
}

// std: yoke-sdk-c:the-interface-library.09
static void test_a_panel_built_on_the_library_names_nothing_of_the_administrative_contract(void) {
    const char *files[] = {SOURCE_DIR "/include/yoke/interface.h", SOURCE_DIR "/interface/interface.c",
                           SOURCE_DIR "/interface/tls.c", SOURCE_DIR "/interface/tls.h"};
    for (size_t i = 0; i < sizeof files / sizeof *files; i++) {
        char *code = code_of_file(files[i]);
        REQUIRE(code != NULL, "%s cannot be read", files[i]);
        bool names = false;
        for (char *at = code; (at = strcasestr(at, "administrative")) != NULL; at++) {
            names = true;
        }
        free(code);
        REQUIRE(!names, "%s names the administrative contract", files[i]);
    }
done:;
}

int main(void) {
    static const check_case cases[] = {
        {"yoke-sdk-c:the-interface-library.01",
         test_a_channels_address_is_computed_or_handed_to_a_managed_interface},
        {"yoke-sdk-c:the-interface-library.02",
         test_attaching_surfaces_the_opening_and_a_version_it_does_not_speak_is_refused},
        {"yoke-sdk-c:the-interface-library.03",
         test_one_function_per_operation_each_stating_the_version_on_one_connection},
        {"yoke-sdk-c:the-interface-library.04", test_a_refusal_is_an_error_with_its_code_and_its_detail},
        {"yoke-sdk-c:the-interface-library.05",
         test_a_subscription_is_delivered_in_order_until_ended_and_nothing_reconnects},
        {"yoke-sdk-c:the-interface-library.06", test_confirmation_is_sent_only_when_the_author_confirms},
        {"yoke-sdk-c:the-interface-library.07",
         test_a_streams_delivery_is_read_where_its_answer_says_it_arrives},
        {"yoke-sdk-c:the-interface-library.08", test_closing_an_attachment_ends_it_in_order},
        {"yoke-sdk-c:the-interface-library.09",
         test_a_panel_built_on_the_library_names_nothing_of_the_administrative_contract},
    };
    return check_run(cases, sizeof cases / sizeof *cases);
}
