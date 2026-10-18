/*
 * The interface harness's cases: a control socket of the case's own stands in for the suite, and a Core
 * serving the channel `panel` under an instance's root stands in for the Core. The harness runs in a thread
 * of the case's process, so that its exit is the case's to observe.
 */
#include <json-c/json.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include "check.h"
#include "core.h"
#include "interface_harness.h"

/* The suite's side: the socket it listens on, the harness's connection, and what is left to read. */
typedef struct suite {
    char path[108], instance[200];
    int listening, conn;
    char buf[65536];
    size_t have;
    pthread_t harness;
    int status;
    bool running;
} suite;

static const char *suite_getenv(const char *name, void *context) {
    suite *s = context;
    if (strcmp(name, "CONFORMANCE_SOCKET") == 0) {
        return s->path;
    }
    if (strcmp(name, "CONFORMANCE_INSTANCE") == 0) {
        return s->instance;
    }
    return NULL;
}

static void *run_harness(void *arg) {
    suite *s = arg;
    s->status = interface_harness_serve(suite_getenv, s);
    s->running = false;
    return NULL;
}

/* Listens, starts the harness, and takes its connection. */
static bool suite_start(suite *s) {
    memset(s, 0, sizeof *s);
    snprintf(s->instance, sizeof s->instance, "%s/instance", check_dir());
    mkdir(s->instance, 0700);
    char interfaces[300];
    snprintf(interfaces, sizeof interfaces, "%s/interfaces", s->instance);
    mkdir(interfaces, 0700);
    snprintf(s->path, sizeof s->path, "%s/suite.sock", check_dir());
    unlink(s->path);
    s->listening = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un addr = {.sun_family = AF_UNIX};
    strcpy(addr.sun_path, s->path);
    if (bind(s->listening, (struct sockaddr *)&addr, sizeof addr) != 0 || listen(s->listening, 1) != 0) {
        check_fail("the suite's socket cannot be bound");
        return false;
    }
    s->running = true;
    pthread_create(&s->harness, NULL, run_harness, s);
    struct pollfd p = {.fd = s->listening, .events = POLLIN};
    if (poll(&p, 1, 5000) != 1) {
        check_fail("the harness never connected");
        return false;
    }
    s->conn = accept(s->listening, NULL, NULL);
    return s->conn >= 0;
}

static json_object *suite_read(suite *s, int timeout_ms) {
    for (long long until = check_ms() + timeout_ms;;) {
        char *nl = memchr(s->buf, '\n', s->have);
        if (nl) {
            *nl = '\0';
            json_object *o = json_tokener_parse(s->buf);
            s->have -= (size_t)(nl + 1 - s->buf);
            memmove(s->buf, nl + 1, s->have);
            return o;
        }
        int left = (int)(until - check_ms());
        struct pollfd p = {.fd = s->conn, .events = POLLIN};
        if (left <= 0 || poll(&p, 1, left) != 1) {
            return NULL;
        }
        ssize_t n = read(s->conn, s->buf + s->have, sizeof s->buf - s->have);
        if (n <= 0) {
            return NULL;
        }
        s->have += (size_t)n;
    }
}

static void suite_write(suite *s, const char *line) {
    ssize_t ignored = write(s->conn, line, strlen(line));
    ignored = write(s->conn, "\n", 1);
    (void)ignored;
}

static bool suite_exited(suite *s, int timeout_ms, int *status) {
    for (long long until = check_ms() + timeout_ms; s->running && check_ms() < until;) {
        check_sleep(20);
    }
    if (s->running) {
        return false;
    }
    pthread_join(s->harness, NULL);
    *status = s->status;
    return true;
}

static const char *str(json_object *o, const char *path) {
    char copy[128];
    snprintf(copy, sizeof copy, "%s", path);
    json_object *at = o;
    for (char *save = NULL, *key = strtok_r(copy, ".", &save); key && at; key = strtok_r(NULL, ".", &save)) {
        if (json_object_is_type(at, json_type_array)) {
            at = json_object_array_get_idx(at, atoi(key));
        } else if (!json_object_object_get_ex(at, key, &at)) {
            return NULL;
        }
    }
    return at ? json_object_get_string(at) : NULL;
}

static bool is(json_object *o, const char *path, const char *want) {
    const char *got = str(o, path);
    return got && strcmp(got, want) == 0;
}

/* Reads lines until the result of the directive id, skipping the hello and what is observed. */
static json_object *result_of(suite *s, const char *id) {
    for (json_object *o; (o = suite_read(s, 5000));) {
        if (is(o, "type", "result") && is(o, "id", id)) {
            return o;
        }
        json_object_put(o);
    }
    return NULL;
}

static void direct(suite *s, const char *id, const char *verb, const char *args) {
    char line[512];
    snprintf(line, sizeof line, "{\"type\":\"directive\",\"id\":\"%s\",\"verb\":\"%s\",\"args\":%s}", id,
             verb, args);
    suite_write(s, line);
}

/* ---- the Cores ---- */

static Yoke__Interface__V1__Record *channel_record(const char *name) {
    Yoke__Interface__V1__Record *r = malloc(sizeof *r);
    yoke__interface__v1__record__init(r);
    Yoke__Interface__V1__ChannelRecord *c = malloc(sizeof *c);
    yoke__interface__v1__channel_record__init(c);
    Yoke__Interface__V1__ChannelRecord__Declared *d = malloc(sizeof *d);
    yoke__interface__v1__channel_record__declared__init(d);
    d->name = strdup(name);
    d->projection = strdup("local");
    d->address_class = strdup("local-socket");
    d->clients = strdup("single");
    Yoke__Interface__V1__ChannelRecord__Observed *o = malloc(sizeof *o);
    yoke__interface__v1__channel_record__observed__init(o);
    o->attached = 1;
    c->declared = d;
    c->observed = o;
    r->subject_case = YOKE__INTERFACE__V1__RECORD__SUBJECT_CHANNEL;
    r->channel = c;
    return r;
}

static void say_event(core_conn *c, uint64_t seq) {
    Yoke__Interface__V1__Event e = YOKE__INTERFACE__V1__EVENT__INIT;
    Yoke__Interface__V1__Subject subject = YOKE__INTERFACE__V1__SUBJECT__INIT;
    subject.kind = (char *)"unit";
    subject.identity = (char *)"acquire";
    e.seq = seq;
    e.type = (char *)"unit.state.changed";
    e.subject = &subject;
    e.severity = 30;
    Yoke__Interface__V1__CoreFrame f = YOKE__INTERFACE__V1__CORE_FRAME__INIT;
    f.call = (char *)"standing";
    f.carries_case = YOKE__INTERFACE__V1__CORE_FRAME__CARRIES_EVENT;
    f.event = &e;
    core_send(c, &f);
}

static const Yoke__Interface__V1__Request *request_of(const Yoke__Interface__V1__ClientFrame *f) {
    return f->carries_case == YOKE__INTERFACE__V1__CLIENT_FRAME__CARRIES_REQUEST ? f->request : NULL;
}

static void script_opens_with_events(core_conn *c, void *arg) {
    (void)arg;
    Yoke__Interface__V1__Record *records[1] = {channel_record("panel")};
    core_send(c, core_opening("standing", 1, records, 1));
    say_event(c, 5);
    say_event(c, 6);
    const Yoke__Interface__V1__ClientFrame *f;
    while (core_next(c, &f, -1) == CORE_FRAME) {
    }
}

static void script_refuses(core_conn *c, void *arg) {
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
            suspension.by = (char *)"bench";
            refusal.detail_case = YOKE__INTERFACE__V1__REFUSAL__DETAIL_SUSPENSION;
            refusal.suspension = &suspension;
        }
        core_send(c, core_refusal_frame(f->call, &refusal));
    }
}

static core *serve_panel(suite *s, core_script script) {
    char interfaces[300];
    snprintf(interfaces, sizeof interfaces, "%s/interfaces", s->instance);
    return core_start(interfaces, script, NULL);
}

// std: yoke-sdk-c:the-interface-harness.01
static void test_hello_first_for_the_interface_contract(void) {
    suite s;
    json_object *hello = NULL;
    int status = -1;
    REQUIRE(suite_start(&s), "the harness did not connect");
    hello = suite_read(&s, 5000);
    char version[16];
    snprintf(version, sizeof version, "%d", yoke_interface_contract());
    REQUIRE(hello && is(hello, "type", "hello") && is(hello, "contract", "interface") &&
                is(hello, "language", "c") && is(hello, "sdk", YOKE_SDK_LINE) &&
                is(hello, "version", version) && str(hello, "unit") == NULL,
            "the first line is %s", hello ? json_object_to_json_string(hello) : "(none)");
    suite_write(&s, "{\"type\":\"finish\"}");
    REQUIRE(suite_exited(&s, 5000, &status) && status == 0, "the harness did not exit zero");
done:
    json_object_put(hello);
}

// std: yoke-sdk-c:the-interface-harness.02
static void test_attaching_reports_the_opening_and_the_standing_subscription_in_order(void) {
    suite s;
    json_object *r = NULL, *a = NULL, *b = NULL;
    REQUIRE(suite_start(&s), "the harness did not connect");
    serve_panel(&s, script_opens_with_events);
    direct(&s, "a-1", "attach", "{\"channel\":\"panel\"}");
    r = result_of(&s, "a-1");
    REQUIRE(r && is(r, "value.version", "1") && is(r, "value.subscription", "standing") &&
                is(r, "value.at", "4") && is(r, "value.records.0.channel.declared.name", "panel"),
            "the attach result is %s", r ? json_object_to_json_string(r) : "(none)");
    a = suite_read(&s, 5000);
    b = suite_read(&s, 5000);
    REQUIRE(a && b && is(a, "type", "observation") && is(a, "kind", "event") && is(a, "fields.seq", "5") &&
                is(a, "fields.type", "unit.state.changed") && is(a, "fields.subject", "acquire") &&
                is(b, "kind", "event") && is(b, "fields.seq", "6"),
            "the observations were %s then %s", a ? json_object_to_json_string(a) : "(none)",
            b ? json_object_to_json_string(b) : "(none)");
    suite_write(&s, "{\"type\":\"finish\"}");
    int status;
    suite_exited(&s, 5000, &status);
done:
    json_object_put(r);
    json_object_put(a);
    json_object_put(b);
}

// std: yoke-sdk-c:the-interface-harness.03
static void test_a_refusal_is_reported_as_its_code_and_what_it_names(void) {
    suite s;
    json_object *read = NULL, *command = NULL, *attached = NULL;
    REQUIRE(suite_start(&s), "the harness did not connect");
    serve_panel(&s, script_refuses);
    direct(&s, "a-1", "attach", "{\"channel\":\"panel\"}");
    attached = result_of(&s, "a-1");
    REQUIRE(attached != NULL, "attach had no result");
    direct(&s, "r-1", "read", "{\"kind\":\"unit\",\"identity\":\"nobody\"}");
    read = result_of(&s, "r-1");
    REQUIRE(read && is(read, "refusal", "subject.unknown") && is(read, "value.subject.kind", "unit") &&
                is(read, "value.subject.identity", "nobody"),
            "the read's result is %s", read ? json_object_to_json_string(read) : "(none)");
    direct(&s, "c-1", "command", "{\"unit\":\"acquire\",\"type\":\"calibrate\",\"payload\":\"\"}");
    command = result_of(&s, "c-1");
    REQUIRE(command && is(command, "refusal", "channel.suspended") &&
                is(command, "value.suspension.grade", "read-only") &&
                is(command, "value.suspension.by", "bench"),
            "the command's result is %s", command ? json_object_to_json_string(command) : "(none)");
    REQUIRE(strstr(json_object_to_json_string(read), "no such unit") == NULL &&
                strstr(json_object_to_json_string(command), "\"message\"") == NULL,
            "a result carries the library's words");
    suite_write(&s, "{\"type\":\"finish\"}");
    int status;
    suite_exited(&s, 5000, &status);
done:
    json_object_put(read);
    json_object_put(command);
    json_object_put(attached);
}

// std: yoke-sdk-c:the-interface-harness.04
static void test_a_verb_it_does_not_know_is_unrecognised_and_it_holds_no_wire(void) {
    suite s;
    json_object *r = NULL;
    int status = -1;
    REQUIRE(suite_start(&s), "the harness did not connect");
    direct(&s, "e-1", "emit", "{\"stream\":\"x\",\"payload\":\"y\"}");
    r = result_of(&s, "e-1");
    REQUIRE(r && is(r, "unrecognised", "true"), "emit was %s", r ? json_object_to_json_string(r) : "(none)");
    suite_write(&s, "{\"type\":\"finish\"}");
    REQUIRE(suite_exited(&s, 5000, &status) && status == 0, "the harness did not exit zero");
    const char *files[] = {SOURCE_DIR "/interface-harness/interface_harness.c",
                           SOURCE_DIR "/interface-harness/interface_harness.h"};
    for (size_t i = 0; i < sizeof files / sizeof *files; i++) {
        char *text = check_read(files[i]);
        REQUIRE(text != NULL, "%s cannot be read", files[i]);
        bool wire = strstr(text, "#include <grpc/") != NULL ||
                    strstr(text, "protobuf_c_message_pack") != NULL || strstr(text, "grpc_call") != NULL;
        free(text);
        REQUIRE(!wire, "%s speaks the wire itself", files[i]);
    }
done:
    json_object_put(r);
}

// std: yoke-sdk-c:the-interface-harness.05
static void test_the_suite_is_run_against_it_as_against_the_plugin_harness(void) {
    char *script = check_read(SOURCE_DIR "/ci/conformance.sh");
    REQUIRE(script != NULL, "ci/conformance.sh cannot be read");
    REQUIRE(strstr(script, "yoke-c-interface-harness") != NULL &&
                strstr(script, "yoke-c-plugin-harness") != NULL,
            "the script does not name both harnesses");
    REQUIRE(strstr(script, "run_suite \"$root/build/yoke-c-interface-harness\"") != NULL,
            "the script does not run the suite against the interface harness");
done:
    free(script);
}

int main(void) {
    static const check_case cases[] = {
        {"yoke-sdk-c:the-interface-harness.01", test_hello_first_for_the_interface_contract},
        {"yoke-sdk-c:the-interface-harness.02",
         test_attaching_reports_the_opening_and_the_standing_subscription_in_order},
        {"yoke-sdk-c:the-interface-harness.03", test_a_refusal_is_reported_as_its_code_and_what_it_names},
        {"yoke-sdk-c:the-interface-harness.04",
         test_a_verb_it_does_not_know_is_unrecognised_and_it_holds_no_wire},
        {"yoke-sdk-c:the-interface-harness.05",
         test_the_suite_is_run_against_it_as_against_the_plugin_harness},
    };
    return check_run(cases, sizeof cases / sizeof *cases);
}
