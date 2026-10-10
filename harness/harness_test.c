/*
 * The harness's cases: a control socket of the case's own stands in for the suite, and a plugin channel
 * for the Core. The harness runs in a thread of the case's process, so that what it does to the process —
 * a termination signal, its exit — is the case's to observe.
 */
#include <json-c/json.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include "channel.h"
#include "check.h"
#include "harness.h"

/* The suite's side: the socket it listens on, the harness's connection, and what is left to read. */
typedef struct suite {
    char path[108];
    int listening, conn;
    char buf[65536];
    size_t have;
    channel *ch;
    pthread_t harness;
    int status;
    bool running;
} suite;

static const char *suite_getenv(const char *name, void *context) {
    suite *s = context;
    if (strcmp(name, "CONFORMANCE_SOCKET") == 0) {
        return s->path;
    }
    if (strcmp(name, "YOKE_UNIT") == 0) {
        return "harness";
    }
    return s->ch ? channel_getenv(name, s->ch) : NULL;
}

static void *run_harness(void *arg) {
    suite *s = arg;
    s->status = harness_serve(suite_getenv, s);
    s->running = false;
    return NULL;
}

/* Listens, starts the harness, and takes its connection. */
static bool suite_start(suite *s, channel *ch) {
    memset(s, 0, sizeof *s);
    s->ch = ch;
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

/* The next line the harness wrote, parsed; NULL when none came within timeout_ms. */
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

/* Whether the harness exited, within timeout_ms, and with what status. */
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
        if (!json_object_object_get_ex(at, key, &at)) {
            return NULL;
        }
    }
    return at ? json_object_get_string(at) : NULL;
}

static bool is(json_object *o, const char *path, const char *want) {
    const char *got = str(o, path);
    return got && strcmp(got, want) == 0;
}

/* Reads lines until the result of the directive id, skipping the hello. */
static json_object *result_of(suite *s, const char *id) {
    for (json_object *o; (o = suite_read(s, 5000));) {
        if (is(o, "type", "result") && is(o, "id", id)) {
            return o;
        }
        json_object_put(o);
    }
    return NULL;
}

static void revoke_session(channel *ch, const char *line) {
    Yoke__Plugin__V1__SessionMessage__Revoked revoked = YOKE__PLUGIN__V1__SESSION_MESSAGE__REVOKED__INIT;
    revoked.cause = YOKE__PLUGIN__V1__SESSION_MESSAGE__REVOKED__CAUSE__CAUSE_PLUGIN_DISABLED;
    revoked.line = (char *)line;
    Yoke__Plugin__V1__SessionMessage session = YOKE__PLUGIN__V1__SESSION_MESSAGE__INIT;
    session.kind_case = YOKE__PLUGIN__V1__SESSION_MESSAGE__KIND_REVOKED;
    session.revoked = &revoked;
    Yoke__Plugin__V1__Envelope e = YOKE__PLUGIN__V1__ENVELOPE__INIT;
    e.payload_case = YOKE__PLUGIN__V1__ENVELOPE__PAYLOAD_SESSION;
    e.session = &session;
    char id[32];
    channel_send(ch, &e, id);
}

/* Starts the harness against a channel on the default terms, and its unit with it. */
static bool started(suite *s, channel **ch) {
    *ch = channel_start(check_dir(), NULL);
    if (!suite_start(s, *ch)) {
        return false;
    }
    suite_write(s, "{\"type\":\"directive\",\"id\":\"s\",\"verb\":\"start\"}");
    json_object *r = result_of(s, "s");
    bool ok = r && is(r, "value.outcome", "accepted");
    if (!ok) {
        check_fail("the unit did not start: %s", r ? json_object_to_json_string(r) : "(no result)");
    }
    json_object_put(r);
    return ok && channel_await(*ch, YOKE__PLUGIN__V1__ENVELOPE__PAYLOAD_SESSION, 0, 5000) >= 0;
}

// std: yoke-sdk-c:the-harness.01
static void test_hello_first_with_what_the_library_declares(void) {
    suite s;
    json_object *hello = NULL;
    REQUIRE(suite_start(&s, NULL), "the harness did not connect");
    hello = suite_read(&s, 5000);
    char version[16];
    snprintf(version, sizeof version, "%d", yoke_plugin_contract());
    REQUIRE(hello && is(hello, "type", "hello") && is(hello, "contract", "plugin") &&
                is(hello, "language", "c") && is(hello, "sdk", YOKE_SDK_LINE) &&
                is(hello, "version", version) && is(hello, "unit", "harness"),
            "the first line is %s", hello ? json_object_to_json_string(hello) : "(none)");
done:
    json_object_put(hello);
}

// std: yoke-sdk-c:the-harness.02
static void test_describe_answers_with_the_manifest_the_library_generates(void) {
    suite s;
    json_object *r = NULL;
    char *manifest = yoke_manifest(harness_declaration());
    REQUIRE(suite_start(&s, NULL), "the harness did not connect");
    suite_write(&s, "{\"type\":\"directive\",\"id\":\"d-1\",\"verb\":\"describe\"}");
    r = result_of(&s, "d-1");
    REQUIRE(r && is(r, "value.manifest", manifest), "describe answered %s",
            r ? json_object_to_json_string(r) : "(nothing)");
done:
    free(manifest);
    json_object_put(r);
}

// std: yoke-sdk-c:the-harness.03
static void test_start_reports_what_admission_answered_and_a_refusal_as_its_code(void) {
    char *granted_streams[] = {"conformance.data"};
    char *withheld_streams[] = {"conformance.frames"};
    Yoke__Plugin__V1__Surface granted = YOKE__PLUGIN__V1__SURFACE__INIT;
    Yoke__Plugin__V1__Surface withheld = YOKE__PLUGIN__V1__SURFACE__INIT;
    granted.n_streams = 1, granted.streams = granted_streams;
    withheld.n_streams = 1, withheld.streams = withheld_streams;
    channel_terms restricted = {
        .outcome = YOKE__PLUGIN__V1__REGISTER_RESPONSE__OUTCOME__OUTCOME_ACCEPTED_WITH_RESTRICTIONS,
        .granted = &granted,
        .withheld = &withheld,
    };
    suite s;
    json_object *r = NULL;
    channel *ch = channel_start(check_dir(), &restricted);
    REQUIRE(suite_start(&s, ch), "the harness did not connect");
    suite_write(&s, "{\"type\":\"directive\",\"id\":\"s\",\"verb\":\"start\"}");
    r = result_of(&s, "s");
    REQUIRE(r && is(r, "value.outcome", "accepted with restrictions"), "start answered %s",
            r ? json_object_to_json_string(r) : "(nothing)");
    const char *text = json_object_to_json_string_ext(r, JSON_C_TO_STRING_PLAIN);
    REQUIRE(strstr(text, "\"granted\":{") && strstr(strstr(text, "\"granted\""), "conformance.data") &&
                strstr(strstr(text, "\"withheld\""), "conformance.frames"),
            "the granted and withheld items are not named: %s", text);
    json_object_put(r);
    r = NULL;
    int status;
    suite_write(&s, "{\"type\":\"finish\"}");
    REQUIRE(suite_exited(&s, 3000, &status), "the first harness did not leave");

    // A second harness, against a channel that refuses, in a directory of its own.
    channel_terms refused = {
        .outcome = YOKE__PLUGIN__V1__REGISTER_RESPONSE__OUTCOME__OUTCOME_REFUSED,
        .stage = YOKE__PLUGIN__V1__STAGE__STAGE_AUTHENTICATION,
        .code = "admission.auth.consumed",
        .message = "the token was already used",
    };
    char second[256];
    snprintf(second, sizeof second, "%s/refused", check_dir());
    mkdir(second, 0700);
    ch = channel_start(second, &refused);
    REQUIRE(suite_start(&s, ch), "the second harness did not connect");
    suite_write(&s, "{\"type\":\"directive\",\"id\":\"s\",\"verb\":\"start\"}");
    r = result_of(&s, "s");
    REQUIRE(r && is(r, "refusal", "admission.auth.consumed") && is(r, "value.stage", "authentication"),
            "a refused start answered %s", r ? json_object_to_json_string(r) : "(nothing)");
    REQUIRE(!strstr(json_object_to_json_string(r), "already used"),
            "the library's message reached the suite");
done:
    json_object_put(r);
}

// std: yoke-sdk-c:the-harness.04
static void test_a_verb_it_does_not_know_is_reported_as_unrecognised(void) {
    suite s;
    json_object *r = NULL;
    REQUIRE(suite_start(&s, NULL), "the harness did not connect");
    suite_write(&s, "{\"type\":\"directive\",\"id\":\"u-1\",\"verb\":\"subscribe\"}");
    r = result_of(&s, "u-1");
    REQUIRE(r && is(r, "unrecognised", "true"), "subscribe answered %s",
            r ? json_object_to_json_string(r) : "(nothing)");
done:
    json_object_put(r);
}

static void send_command(channel *ch, const char *type) {
    Yoke__Plugin__V1__Control__Command command = YOKE__PLUGIN__V1__CONTROL__COMMAND__INIT;
    command.type = (char *)type;
    Yoke__Plugin__V1__Control control = YOKE__PLUGIN__V1__CONTROL__INIT;
    control.kind_case = YOKE__PLUGIN__V1__CONTROL__KIND_COMMAND;
    control.command = &command;
    Yoke__Plugin__V1__Envelope e = YOKE__PLUGIN__V1__ENVELOPE__INIT;
    e.payload_case = YOKE__PLUGIN__V1__ENVELOPE__PAYLOAD_CONTROL;
    e.control = &control;
    char id[32];
    channel_send(ch, &e, id);
}

/* The next observation, skipping results. */
static json_object *observation(suite *s) {
    for (json_object *o; (o = suite_read(s, 5000));) {
        if (is(o, "type", "observation")) {
            return o;
        }
        json_object_put(o);
    }
    return NULL;
}

// std: yoke-sdk-c:the-harness.05
static void test_what_the_library_surfaces_is_observed_in_order_and_the_end_ends_it(void) {
    suite s;
    channel *ch = NULL;
    json_object *first = NULL, *second = NULL;
    int status = -1;
    REQUIRE(started(&s, &ch), "the harness's unit did not start");
    send_command(ch, "calibrate");
    revoke_session(ch, "the operator disabled the plugin");
    first = observation(&s);
    second = observation(&s);
    REQUIRE(first && is(first, "kind", "command") && is(first, "fields.type", "calibrate"),
            "the first observation is %s", first ? json_object_to_json_string(first) : "(none)");
    REQUIRE(second && is(second, "kind", "session-ended") && is(second, "fields.cause", "plugin disabled") &&
                is(second, "fields.closed", "false"),
            "the second observation is %s", second ? json_object_to_json_string(second) : "(none)");
    REQUIRE(suite_exited(&s, 3000, &status) && status == 0, "the harness did not exit with 0 after the end");
done:
    json_object_put(first);
    json_object_put(second);
}

// std: yoke-sdk-c:the-harness.06
static void test_finish_ends_the_harness_which_holds_no_wire(void) {
    suite s;
    channel *ch = NULL;
    int status = -1;
    char *code = NULL, *header = NULL, *build = NULL;
    REQUIRE(started(&s, &ch), "the harness's unit did not start");
    suite_write(&s, "{\"type\":\"finish\"}");
    REQUIRE(suite_exited(&s, 3000, &status) && status == 0, "the harness did not exit with 0 on finish");
    code = check_read(SOURCE_DIR "/harness/harness.c");
    header = check_read(SOURCE_DIR "/harness/harness.h");
    build = check_read(SOURCE_DIR "/CMakeLists.txt");
    REQUIRE(code && header && build, "the harness's source cannot be read");
    for (const char *wire[] = {"pb-c.h", "grpc/", "protobuf-c/", NULL}, **w = wire; *w; w++) {
        REQUIRE(!strstr(code, *w) && !strstr(header, *w), "the harness includes %s", *w);
    }
    const char *linked = strstr(build, "target_link_libraries(yoke-c-plugin-harness");
    REQUIRE(linked, "the harness's links cannot be read");
    const char *end = strchr(linked, ')');
    REQUIRE(end && !memmem(linked, (size_t)(end - linked), "GRPC", 4) &&
                !memmem(linked, (size_t)(end - linked), "PROTOBUF", 8),
            "the harness links a wire of its own");
done:
    free(code);
    free(header);
    free(build);
}

// std: yoke-sdk-c:the-harness.07
static void test_asked_to_stop_it_reports_the_end_first_then_leaves(void) {
    suite s;
    channel *ch = NULL;
    json_object *ended = NULL;
    int status = -1;
    REQUIRE(started(&s, &ch), "the harness's unit did not start");
    kill(getpid(), SIGTERM);
    check_sleep(200);
    revoke_session(ch, "the Core is stopping");
    ended = observation(&s);
    REQUIRE(ended && is(ended, "kind", "session-ended"), "the end was not reported: %s",
            ended ? json_object_to_json_string(ended) : "(none)");
    REQUIRE(suite_exited(&s, 3000, &status) && status == 0, "the harness did not exit with 0");
done:
    json_object_put(ended);
}

// std: yoke-sdk-c:the-harness.08
static void test_the_suite_is_the_published_pair_authenticated_and_never_built(void) {
    char *script = check_read(SOURCE_DIR "/ci/conformance.sh");
    char *workflow = check_read(SOURCE_DIR "/.github/workflows/verify.yml");
    REQUIRE(script, "no ci/conformance.sh");
    REQUIRE(strstr(script, "yoke-conformance-") && strstr(script, "releases/download"),
            "the script does not download the published archive");
    REQUIRE(strstr(script, "manifest.jsonl") && strstr(script, "sha256sum"),
            "the script does not authenticate it by the manifest");
    REQUIRE(!strstr(script, "go install") && !strstr(script, "go build") && !strstr(script, "go run"),
            "the script builds yoke");
    REQUIRE(workflow, "no workflow");
    const char *test = strstr(workflow, "run: just test"),
               *conformance = strstr(workflow, "run: ci/conformance.sh");
    REQUIRE(test && conformance && conformance > test, "the workflow does not run the suite after just test");
done:
    free(script);
    free(workflow);
}

static void send_question(channel *ch, const char *type, const char *payload) {
    Yoke__Plugin__V1__Query__Question question = YOKE__PLUGIN__V1__QUERY__QUESTION__INIT;
    question.type = (char *)type;
    question.payload.data = (uint8_t *)payload;
    question.payload.len = strlen(payload);
    Yoke__Plugin__V1__Query query = YOKE__PLUGIN__V1__QUERY__INIT;
    query.kind_case = YOKE__PLUGIN__V1__QUERY__KIND_QUESTION;
    query.question = &question;
    Yoke__Plugin__V1__Envelope e = YOKE__PLUGIN__V1__ENVELOPE__INIT;
    e.payload_case = YOKE__PLUGIN__V1__ENVELOPE__PAYLOAD_QUERY;
    e.query = &query;
    char id[32];
    channel_send(ch, &e, id);
}

// std: yoke-sdk-c:the-harness.09
static void test_a_question_is_observed_with_the_bytes_it_carries(void) {
    suite s;
    channel *ch = NULL;
    json_object *q = NULL;
    REQUIRE(started(&s, &ch), "the harness's unit did not start");
    send_question(ch, "status", "how are you");
    q = observation(&s);
    REQUIRE(q && is(q, "kind", "question") && is(q, "fields.type", "status") &&
                is(q, "fields.payload", "how are you") && str(q, "fields.id") && *str(q, "fields.id"),
            "the question was observed as %s", q ? json_object_to_json_string(q) : "(nothing)");
done:
    json_object_put(q);
}

// std: yoke-sdk-c:the-harness.10
static void test_it_declares_a_stream_on_each_transport_each_governed(void) {
    const yoke_declaration *d = harness_declaration();
    const yoke_stream *ordered = NULL, *framed = NULL;
    for (const yoke_stream *s = d->streams; s && s->id; s++) {
        if (!s->tolerates_loss && !s->tolerates_reorder) {
            ordered = s;
        } else if (s->tolerates_loss) {
            framed = s;
        }
    }
    REQUIRE(ordered && framed, "it does not declare a stream on each transport");
    for (const yoke_stream *want[] = {ordered, framed, NULL}, **w = want; *w; w++) {
        bool governed = false;
        for (const yoke_capability *c = d->capabilities; c && c->name; c++) {
            governed =
                governed || (c->governs.kind == YOKE_GOVERNS_STREAM && strcmp(c->governs.id, (*w)->id) == 0);
        }
        REQUIRE(governed, "the stream %s is governed by no capability", (*w)->id);
    }
done:;
}

int main(void) {
    static const check_case cases[] = {
        {"yoke-sdk-c:the-harness.01", test_hello_first_with_what_the_library_declares},
        {"yoke-sdk-c:the-harness.02", test_describe_answers_with_the_manifest_the_library_generates},
        {"yoke-sdk-c:the-harness.03", test_start_reports_what_admission_answered_and_a_refusal_as_its_code},
        {"yoke-sdk-c:the-harness.04", test_a_verb_it_does_not_know_is_reported_as_unrecognised},
        {"yoke-sdk-c:the-harness.05",
         test_what_the_library_surfaces_is_observed_in_order_and_the_end_ends_it},
        {"yoke-sdk-c:the-harness.06", test_finish_ends_the_harness_which_holds_no_wire},
        {"yoke-sdk-c:the-harness.07", test_asked_to_stop_it_reports_the_end_first_then_leaves},
        {"yoke-sdk-c:the-harness.08", test_the_suite_is_the_published_pair_authenticated_and_never_built},
        {"yoke-sdk-c:the-harness.09", test_a_question_is_observed_with_the_bytes_it_carries},
        {"yoke-sdk-c:the-harness.10", test_it_declares_a_stream_on_each_transport_each_governed},
    };
    return check_run(cases, sizeof cases / sizeof *cases);
}
