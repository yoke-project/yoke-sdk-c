#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "channel.h"
#include "check.h"
#include "yoke/plugin.h"

/* The declaration the cases start a unit with. */
static const yoke_declaration station = {
    .id = "com.yoke.station.acquire",
    .needs = (const char *const[]){"device:instrument", NULL},
    .streams = (const yoke_stream[]){{"station.spectra", false, false}, {"station.preview", true, true}, {0}},
    .commands = (const char *const[]){"calibrate", NULL},
    .queries = (const char *const[]){"head-status", NULL},
    .occurrences = (const char *const[]){"calibration.drift", NULL},
    .capabilities =
        (const yoke_capability[]){
            {"acquire.spectra", {YOKE_GOVERNS_STREAM, "station.spectra"}},
            {"acquire.preview", {YOKE_GOVERNS_STREAM, "station.preview"}},
            {"acquire.calibrate", {YOKE_GOVERNS_COMMAND, "calibrate"}},
            {"acquire.status", {YOKE_GOVERNS_QUERY, "head-status"}},
            {"acquire.drift", {YOKE_GOVERNS_OCCURRENCE, "calibration.drift"}},
            {0},
        },
};

/* Starts a channel on terms, and a unit against it whose Session is open. */
static yoke_unit *open_unit(channel **ch, const channel_terms *terms) {
    *ch = channel_start(check_dir(), terms);
    yoke_error err = {0};
    yoke_unit *u = yoke_start_with(&station, channel_getenv, *ch, &err);
    if (u == NULL) {
        check_fail("the unit did not start: %s", err.message ? err.message : "(no message)");
        yoke_error_clear(&err);
        return NULL;
    }
    if (channel_await(*ch, YOKE__PLUGIN__V1__ENVELOPE__PAYLOAD_SESSION, 0, 5000) < 0) {
        check_fail("the Session was never opened");
    }
    return u;
}

static void close_all(channel *ch, yoke_unit *u) {
    if (u != NULL) {
        yoke_free(u);
    }
    if (ch != NULL) {
        channel_stop(ch);
    }
}

/* The next event, which must be of the kind given. */
static bool next_of(yoke_unit *u, yoke_event *ev, yoke_event_kind kind) {
    int got = yoke_next_within(u, ev, 5000);
    if (got != 1) {
        check_fail("no event came, where one of kind %d was expected", kind);
        return false;
    }
    if (ev->kind != kind) {
        check_fail("an event of kind %d came, where one of kind %d was expected", ev->kind, kind);
        return false;
    }
    return true;
}

/* The header with its comments removed: what an author can write against. */
static char *header_code(void) {
    char *text = check_read(SOURCE_DIR "/include/yoke/plugin.h");
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

/* The body of the struct a header declares as `typedef struct name {`, or NULL. */
static char *struct_body(const char *code, const char *name) {
    char opener[128];
    snprintf(opener, sizeof opener, "typedef struct %s {", name);
    const char *at = strstr(code, opener);
    if (at == NULL) {
        return NULL;
    }
    at += strlen(opener);
    const char *end = strchr(at, '}');
    return end ? strndup(at, (size_t)(end - at)) : NULL;
}

static bool lists_equal(char **got, size_t n_got, const char *const *want) {
    size_t n = 0;
    while (want[n] != NULL) {
        n++;
    }
    if (n != n_got) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        if (strcmp(got[i], want[i]) != 0) {
            return false;
        }
    }
    return true;
}

// std: yoke-sdk-c:the-plugin-library.01
static void test_a_declaration_generates_the_manifest(void) {
    char want[2048];
    snprintf(want, sizeof want,
             "manifest: 1\n"
             "id: \"com.yoke.station.acquire\"\n"
             "protocol: %d\n"
             "needs:\n"
             "  - \"device:instrument\"\n"
             "streams:\n"
             "  - id: \"station.spectra\"\n"
             "  - id: \"station.preview\"\n"
             "    tolerates_loss: true\n"
             "    tolerates_reorder: true\n"
             "commands:\n"
             "  - id: \"calibrate\"\n"
             "queries:\n"
             "  - id: \"head-status\"\n"
             "occurrences:\n"
             "  - id: \"calibration.drift\"\n"
             "capabilities:\n"
             "  - name: \"acquire.spectra\"\n"
             "    governs:\n"
             "      stream: \"station.spectra\"\n"
             "  - name: \"acquire.preview\"\n"
             "    governs:\n"
             "      stream: \"station.preview\"\n"
             "  - name: \"acquire.calibrate\"\n"
             "    governs:\n"
             "      command: \"calibrate\"\n"
             "  - name: \"acquire.status\"\n"
             "    governs:\n"
             "      query: \"head-status\"\n"
             "  - name: \"acquire.drift\"\n"
             "    governs:\n"
             "      occurrence: \"calibration.drift\"\n",
             yoke_plugin_contract());
    char *got = yoke_manifest(&station);
    REQUIRE(got != NULL, "no Manifest was generated");
    REQUIRE(strcmp(got, want) == 0, "the Manifest is:\n%s", got);
done:
    free(got);
}

// std: yoke-sdk-c:the-plugin-library.02
static void test_nothing_the_model_does_not_have_can_be_declared(void) {
    static const char *const types[] = {"yoke_declaration", "yoke_stream", "yoke_capability", "yoke_object"};
    static const char *const forbidden[] = {"endpoint", "autostart", "digest", "description"};
    char *code = header_code();
    char *body = NULL;
    REQUIRE(code != NULL, "the plugin library's header cannot be read");
    for (size_t i = 0; i < sizeof types / sizeof *types; i++) {
        body = struct_body(code, types[i]);
        REQUIRE(body != NULL, "the header declares no struct %s", types[i]);
        for (size_t j = 0; j < sizeof forbidden / sizeof *forbidden; j++) {
            REQUIRE(strcasestr(body, forbidden[j]) == NULL, "%s has a field for %s", types[i], forbidden[j]);
        }
        free(body);
        body = NULL;
    }
done:
    free(body);
    free(code);
}

// std: yoke-sdk-c:the-plugin-library.03
static void test_the_registration_claims_what_the_manifest_declares(void) {
    channel *ch = NULL;
    yoke_unit *u = open_unit(&ch, NULL);
    REQUIRE(u != NULL, "the unit did not start");
    const Yoke__Plugin__V1__RegisterRequest *r = channel_request(ch);
    REQUIRE(r != NULL, "no registration arrived");
    REQUIRE(strcmp(r->plugin, "com.yoke.station.acquire") == 0, "the plugin is %s", r->plugin);
    REQUIRE(strcmp(r->unit, "acquire") == 0, "the unit is %s", r->unit);
    REQUIRE(strcmp(r->token, "t-0123") == 0, "the token is %s", r->token);
    REQUIRE((int)r->protocol == yoke_plugin_contract(), "the protocol is %u", r->protocol);
    REQUIRE(strcmp(r->language, "c") == 0, "the language is %s", r->language);
    REQUIRE(strcmp(r->sdk_line, YOKE_SDK_LINE) == 0, "the SDK line is %s", r->sdk_line);
    const Yoke__Plugin__V1__Surface *d = r->declared;
    REQUIRE(d != NULL, "nothing was declared");
    static const char *const capabilities[] = {"acquire.spectra", "acquire.preview", "acquire.calibrate",
                                               "acquire.status",  "acquire.drift",   NULL};
    static const char *const streams[] = {"station.spectra", "station.preview", NULL};
    REQUIRE(lists_equal(d->capabilities, d->n_capabilities, capabilities), "the capabilities differ");
    REQUIRE(lists_equal(d->streams, d->n_streams, streams), "the streams differ");
    REQUIRE(lists_equal(d->commands, d->n_commands, station.commands), "the commands differ");
    REQUIRE(lists_equal(d->queries, d->n_queries, station.queries), "the queries differ");
    REQUIRE(lists_equal(d->occurrences, d->n_occurrences, station.occurrences), "the occurrences differ");
    REQUIRE(protobuf_c_message_descriptor_get_field_by_name(&yoke__plugin__v1__register_request__descriptor,
                                                            "incarnation") == NULL,
            "the request has an incarnation");
done:
    close_all(ch, u);
}

// std: yoke-sdk-c:the-plugin-library.04
static void test_the_socket_is_bound_before_registering(void) {
    channel *ch = NULL;
    yoke_unit *u = open_unit(&ch, NULL);
    REQUIRE(u != NULL, "the unit did not start");
    REQUIRE(channel_bound_at_registration(ch), "nothing was bound at %s when the registration arrived",
            channel_bind(ch));
done:
    close_all(ch, u);
}

// std: yoke-sdk-c:the-plugin-library.05
static void test_a_refusal_is_surfaced_and_never_retried(void) {
    channel_terms terms = {
        .outcome = YOKE__PLUGIN__V1__REGISTER_RESPONSE__OUTCOME__OUTCOME_REFUSED,
        .stage = YOKE__PLUGIN__V1__STAGE__STAGE_AUTHENTICATION,
        .code = "admission.auth.consumed",
        .message = "the token was already used",
    };
    channel *ch = channel_start(check_dir(), &terms);
    yoke_error err = {0};
    yoke_unit *u = yoke_start_with(&station, channel_getenv, ch, &err);
    REQUIRE(u == NULL, "the unit started on a refused registration");
    REQUIRE(yoke_is_refusal(&err, "admission.auth.consumed"), "the refusal's code is %s",
            err.code ? err.code : "(none)");
    REQUIRE(err.stage != NULL && strcmp(err.stage, "authentication") == 0, "the refusal's stage is %s",
            err.stage ? err.stage : "(none)");
    check_sleep(300);
    REQUIRE(channel_registrations(ch) == 1, "%d registrations arrived", channel_registrations(ch));
    REQUIRE(channel_sessions(ch) == 0, "%d Sessions were opened", channel_sessions(ch));
done:
    yoke_error_clear(&err);
    close_all(ch, u);
}

// std: yoke-sdk-c:the-plugin-library.06
static void test_an_acceptance_with_restrictions_names_what_was_withheld(void) {
    char *granted_streams[] = {"station.spectra"};
    char *withheld_streams[] = {"station.preview"};
    char *withheld_occurrences[] = {"calibration.drift"};
    Yoke__Plugin__V1__Surface granted = YOKE__PLUGIN__V1__SURFACE__INIT;
    Yoke__Plugin__V1__Surface withheld = YOKE__PLUGIN__V1__SURFACE__INIT;
    granted.n_streams = 1;
    granted.streams = granted_streams;
    withheld.n_streams = 1;
    withheld.streams = withheld_streams;
    withheld.n_occurrences = 1;
    withheld.occurrences = withheld_occurrences;
    channel_terms terms = {
        .outcome = YOKE__PLUGIN__V1__REGISTER_RESPONSE__OUTCOME__OUTCOME_ACCEPTED_WITH_RESTRICTIONS,
        .granted = &granted,
        .withheld = &withheld,
    };
    channel *ch = NULL;
    yoke_unit *u = open_unit(&ch, &terms);
    REQUIRE(u != NULL, "the unit did not start");
    const yoke_admission *a = yoke_admission_of(u);
    REQUIRE(a->restricted, "the unit does not say it was admitted with restrictions");
    REQUIRE(a->granted.streams[0] != NULL && strcmp(a->granted.streams[0], "station.spectra") == 0 &&
                a->granted.streams[1] == NULL,
            "the granted streams are not station.spectra alone");
    REQUIRE(a->withheld.streams[0] != NULL && strcmp(a->withheld.streams[0], "station.preview") == 0,
            "the withheld stream is not named");
    REQUIRE(a->withheld.occurrences[0] != NULL &&
                strcmp(a->withheld.occurrences[0], "calibration.drift") == 0,
            "the withheld occurrence is not named");
done:
    close_all(ch, u);
}

// std: yoke-sdk-c:the-plugin-library.07
static void test_the_session_opens_and_beats_on_the_cores_terms(void) {
    channel_terms terms = {.interval_ms = 100};
    channel *ch = NULL;
    char *code = NULL;
    yoke_unit *u = open_unit(&ch, &terms);
    REQUIRE(u != NULL, "the unit did not start");
    REQUIRE(yoke_health(u, 30, "starting", NULL) == 0, "the author's report was refused");
    check_sleep(450);
    const Yoke__Plugin__V1__Envelope *first = channel_arrival(ch, 0, NULL);
    REQUIRE(first != NULL && first->payload_case == YOKE__PLUGIN__V1__ENVELOPE__PAYLOAD_SESSION &&
                first->session->kind_case == YOKE__PLUGIN__V1__SESSION_MESSAGE__KIND_OPEN,
            "the Session's first envelope is not an OPEN");
    REQUIRE(strcmp(first->session_id, "s-1") == 0, "the OPEN carries the Session %s", first->session_id);
    int beats = -1; /* the author's own report is not a beat */
    long long last = -1;
    for (size_t i = 1; i < channel_count(ch); i++) {
        long long at;
        const Yoke__Plugin__V1__Envelope *e = channel_arrival(ch, i, &at);
        if (e->payload_case != YOKE__PLUGIN__V1__ENVELOPE__PAYLOAD_HEALTH) {
            continue;
        }
        REQUIRE(last < 0 || at - last >= 50, "two health reports arrived %lld ms apart", at - last);
        last = at;
        beats++;
    }
    REQUIRE(beats >= 3, "%d heartbeats followed the OPEN", beats < 0 ? 0 : beats);
    code = header_code();
    REQUIRE(code != NULL, "the plugin library's header cannot be read");
    REQUIRE(strcasestr(code, "interval") == NULL, "an author is given a way to choose the interval");
done:
    free(code);
    close_all(ch, u);
}

static void revoke_session(channel *ch, Yoke__Plugin__V1__SessionMessage__Revoked__Cause cause,
                           const char *line) {
    Yoke__Plugin__V1__SessionMessage__Revoked revoked = YOKE__PLUGIN__V1__SESSION_MESSAGE__REVOKED__INIT;
    revoked.cause = cause;
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

// std: yoke-sdk-c:the-plugin-library.08
static void test_the_end_of_a_session_is_surfaced_and_nothing_reconnects(void) {
    channel *ch = NULL;
    yoke_event ev = {0};
    yoke_unit *u = open_unit(&ch, NULL);
    REQUIRE(u != NULL, "the unit did not start");
    revoke_session(ch, YOKE__PLUGIN__V1__SESSION_MESSAGE__REVOKED__CAUSE__CAUSE_PLUGIN_DISABLED,
                   "the operator disabled the plugin");
    REQUIRE(next_of(u, &ev, YOKE_ENDED), "the end was not surfaced");
    REQUIRE(!ev.closed, "the end is surfaced as a close the unit made");
    REQUIRE(ev.cause != NULL && strcmp(ev.cause, "plugin disabled") == 0, "the cause is %s",
            ev.cause ? ev.cause : "(none)");
    REQUIRE(ev.line != NULL && strcmp(ev.line, "the operator disabled the plugin") == 0, "the line is %s",
            ev.line ? ev.line : "(none)");
    REQUIRE(yoke_done(u), "the unit does not report itself done");
    yoke_event_clear(&ev);
    REQUIRE(yoke_next_within(u, &ev, 100) == 0, "something followed the end");
    check_sleep(500);
    REQUIRE(channel_registrations(ch) == 1, "%d registrations arrived", channel_registrations(ch));
    REQUIRE(channel_sessions(ch) == 1, "%d Sessions were opened", channel_sessions(ch));
done:
    yoke_event_clear(&ev);
    close_all(ch, u);
}

// std: yoke-sdk-c:the-plugin-library.09
static void test_an_orderly_close_is_the_units(void) {
    channel *ch = NULL;
    yoke_event ev = {0};
    yoke_unit *u = open_unit(&ch, NULL);
    REQUIRE(u != NULL, "the unit did not start");
    REQUIRE(yoke_close(u, NULL) == 0, "the close failed");
    long i = channel_await(ch, YOKE__PLUGIN__V1__ENVELOPE__PAYLOAD_SESSION, 1, 3000);
    REQUIRE(i >= 0, "no session message followed the OPEN");
    const Yoke__Plugin__V1__Envelope *e = channel_arrival(ch, (size_t)i, NULL);
    REQUIRE(e->session->kind_case == YOKE__PLUGIN__V1__SESSION_MESSAGE__KIND_CLOSE,
            "the session message that followed is not a CLOSE");
    channel_end(ch);
    REQUIRE(next_of(u, &ev, YOKE_ENDED), "the end was not surfaced");
    REQUIRE(ev.closed, "the end is not surfaced as a close the unit made");
done:
    yoke_event_clear(&ev);
    close_all(ch, u);
}

static void send_command(channel *ch, const char *type, char id[32]) {
    Yoke__Plugin__V1__Control__Command command = YOKE__PLUGIN__V1__CONTROL__COMMAND__INIT;
    command.type = (char *)type;
    Yoke__Plugin__V1__Control control = YOKE__PLUGIN__V1__CONTROL__INIT;
    control.kind_case = YOKE__PLUGIN__V1__CONTROL__KIND_COMMAND;
    control.command = &command;
    Yoke__Plugin__V1__Envelope e = YOKE__PLUGIN__V1__ENVELOPE__INIT;
    e.payload_case = YOKE__PLUGIN__V1__ENVELOPE__PAYLOAD_CONTROL;
    e.control = &control;
    channel_send(ch, &e, id);
}

static void send_question(channel *ch, const char *type, char id[32]) {
    Yoke__Plugin__V1__Query__Question question = YOKE__PLUGIN__V1__QUERY__QUESTION__INIT;
    question.type = (char *)type;
    Yoke__Plugin__V1__Query query = YOKE__PLUGIN__V1__QUERY__INIT;
    query.kind_case = YOKE__PLUGIN__V1__QUERY__KIND_QUESTION;
    query.question = &question;
    Yoke__Plugin__V1__Envelope e = YOKE__PLUGIN__V1__ENVELOPE__INIT;
    e.payload_case = YOKE__PLUGIN__V1__ENVELOPE__PAYLOAD_QUERY;
    e.query = &query;
    channel_send(ch, &e, id);
}

// std: yoke-sdk-c:the-plugin-library.10
static void test_what_the_core_sends_is_surfaced_and_answers_are_correlated(void) {
    channel *ch = NULL;
    yoke_event command = {0}, question = {0};
    char command_id[32], question_id[32];
    yoke_unit *u = open_unit(&ch, NULL);
    REQUIRE(u != NULL, "the unit did not start");
    send_command(ch, "calibrate", command_id);
    send_question(ch, "head-status", question_id);
    REQUIRE(next_of(u, &command, YOKE_COMMAND), "the command was not surfaced first");
    REQUIRE(strcmp(command.type, "calibrate") == 0 && strcmp(command.id, command_id) == 0,
            "the command surfaced is %s, %s", command.type, command.id);
    REQUIRE(next_of(u, &question, YOKE_QUESTION), "the question was not surfaced second");
    REQUIRE(strcmp(question.type, "head-status") == 0 && strcmp(question.id, question_id) == 0,
            "the question surfaced is %s, %s", question.type, question.id);
    REQUIRE(yoke_ack(u, &command, YOKE_DONE, "calibrated", NULL) == 0, "the acknowledgement failed");
    REQUIRE(yoke_answer(u, &question, "nominal", 7, NULL) == 0, "the answer failed");
    long i = channel_await(ch, YOKE__PLUGIN__V1__ENVELOPE__PAYLOAD_ACK, 0, 3000);
    REQUIRE(i >= 0, "no acknowledgement arrived");
    REQUIRE(strcmp(channel_arrival(ch, (size_t)i, NULL)->correlation_id, command_id) == 0,
            "the acknowledgement is not correlated to the command");
    i = channel_await(ch, YOKE__PLUGIN__V1__ENVELOPE__PAYLOAD_QUERY, 0, 3000);
    REQUIRE(i >= 0, "no answer arrived");
    const Yoke__Plugin__V1__Envelope *answer = channel_arrival(ch, (size_t)i, NULL);
    REQUIRE(strcmp(answer->correlation_id, question_id) == 0, "the answer is not correlated to the question");
    REQUIRE(answer->query->kind_case == YOKE__PLUGIN__V1__QUERY__KIND_ANSWER &&
                answer->query->answer->payload.len == 7 &&
                memcmp(answer->query->answer->payload.data, "nominal", 7) == 0,
            "the answer does not carry the payload");
done:
    yoke_event_clear(&command);
    yoke_event_clear(&question);
    close_all(ch, u);
}

// std: yoke-sdk-c:the-plugin-library.11
static void test_an_occurrence_carries_the_authors_severity_or_is_refused(void) {
    channel *ch = NULL;
    yoke_error err = {0};
    yoke_unit *u = open_unit(&ch, NULL);
    REQUIRE(u != NULL, "the unit did not start");
    yoke_severity none = {0};
    REQUIRE(yoke_report(u, "calibration.drift", none, "drifted", NULL, 0, &err) == -1,
            "an occurrence with no severity was reported");
    check_sleep(200);
    REQUIRE(channel_await(ch, YOKE__PLUGIN__V1__ENVELOPE__PAYLOAD_EVENT, 0, 0) < 0,
            "an event reached the channel with no severity stated");
    REQUIRE(yoke_report(u, "calibration.drift", YOKE_SEVERITY(40), "drifted", NULL, 0, NULL) == 0,
            "an occurrence with severity 40 was refused");
    long i = channel_await(ch, YOKE__PLUGIN__V1__ENVELOPE__PAYLOAD_EVENT, 0, 3000);
    REQUIRE(i >= 0, "no event arrived");
    const Yoke__Plugin__V1__Event *e = channel_arrival(ch, (size_t)i, NULL)->event;
    REQUIRE(e->severity == 40 && strcmp(e->occurrence, "calibration.drift") == 0,
            "the event is %s with severity %u", e->occurrence, e->severity);
done:
    yoke_error_clear(&err);
    close_all(ch, u);
}

/* How many entries the case's directory holds. */
static int entries(void) {
    DIR *d = opendir(check_dir());
    int n = 0;
    for (struct dirent *e; d != NULL && (e = readdir(d)) != NULL;) {
        if (strcmp(e->d_name, ".") != 0 && strcmp(e->d_name, "..") != 0) {
            n++;
        }
    }
    if (d != NULL) {
        closedir(d);
    }
    return n;
}

// std: yoke-sdk-c:the-plugin-library.12
static void test_nothing_is_emitted_on_a_stream_not_activated(void) {
    channel *ch = NULL;
    yoke_error err = {0};
    yoke_unit *u = open_unit(&ch, NULL);
    REQUIRE(u != NULL, "the unit did not start");
    int before = entries();
    REQUIRE(yoke_emit(u, "station.spectra", "x", 1, &err) == -1,
            "an emission on an inactive stream succeeded");
    REQUIRE(yoke_is_refusal(&err, "stream.inactive"), "the refusal is %s", err.code ? err.code : "(none)");
    REQUIRE(entries() == before, "something was created beside the channel");
    check_sleep(200);
    REQUIRE(channel_await(ch, YOKE__PLUGIN__V1__ENVELOPE__PAYLOAD_DATA, 0, 0) < 0,
            "data reached the channel");
done:
    yoke_error_clear(&err);
    close_all(ch, u);
}

/* The acknowledgement correlated to id, or NULL. */
static const Yoke__Plugin__V1__Ack *ack_of(channel *ch, const char *id, size_t *from) {
    for (;;) {
        long i = channel_await(ch, YOKE__PLUGIN__V1__ENVELOPE__PAYLOAD_ACK, *from, 3000);
        if (i < 0) {
            return NULL;
        }
        *from = (size_t)i + 1;
        const Yoke__Plugin__V1__Envelope *e = channel_arrival(ch, (size_t)i, NULL);
        if (strcmp(e->correlation_id, id) == 0) {
            return e->ack;
        }
    }
}

// std: yoke-sdk-c:the-plugin-library.13
static void test_every_family_a_unit_originates_has_an_act(void) {
    channel *ch = NULL;
    yoke_event command = {0}, question = {0};
    char command_id[32], question_id[32];
    yoke_unit *u = open_unit(&ch, NULL);
    REQUIRE(u != NULL, "the unit did not start");
    send_command(ch, "calibrate", command_id);
    send_question(ch, "head-status", question_id);
    REQUIRE(next_of(u, &command, YOKE_COMMAND), "the command was not surfaced");
    REQUIRE(next_of(u, &question, YOKE_QUESTION), "the question was not surfaced");
    REQUIRE(yoke_ack(u, &command, YOKE_ACCEPTED, "", NULL) == 0, "the acceptance failed");
    REQUIRE(yoke_ack(u, &command, YOKE_DONE, "calibrated", NULL) == 0, "the completion failed");
    REQUIRE(yoke_fail(u, question.id, "head.unreachable", "the head did not answer", NULL) == 0,
            "the error failed");
    size_t from = 0;
    const Yoke__Plugin__V1__Ack *a = ack_of(ch, command_id, &from);
    REQUIRE(a != NULL && a->outcome == YOKE__PLUGIN__V1__ACK__OUTCOME__OUTCOME_ACCEPTED,
            "the first acknowledgement is not `accepted`");
    a = ack_of(ch, command_id, &from);
    REQUIRE(a != NULL && a->outcome == YOKE__PLUGIN__V1__ACK__OUTCOME__OUTCOME_DONE,
            "the second acknowledgement is not `done`");
    long i = channel_await(ch, YOKE__PLUGIN__V1__ENVELOPE__PAYLOAD_ERROR, 0, 3000);
    REQUIRE(i >= 0, "no error arrived");
    const Yoke__Plugin__V1__Envelope *e = channel_arrival(ch, (size_t)i, NULL);
    REQUIRE(strcmp(e->correlation_id, question_id) == 0, "the error is not correlated to the question");
    REQUIRE(strcmp(e->error->code, "head.unreachable") == 0 &&
                strcmp(e->error->message, "the head did not answer") == 0,
            "the error carries %s, %s", e->error->code, e->error->message);
done:
    yoke_event_clear(&command);
    yoke_event_clear(&question);
    close_all(ch, u);
}

// std: yoke-sdk-c:the-plugin-library.14
static void test_a_beat_repeats_the_authors_last_report(void) {
    channel_terms terms = {.interval_ms = 100};
    channel *ch = NULL;
    yoke_unit *u = open_unit(&ch, &terms);
    REQUIRE(u != NULL, "the unit did not start");
    check_sleep(350);
    REQUIRE(channel_await(ch, YOKE__PLUGIN__V1__ENVELOPE__PAYLOAD_HEALTH, 0, 0) < 0,
            "a health report reached the channel before the author's first");
    size_t first = channel_count(ch);
    REQUIRE(yoke_health(u, 40, "warming", NULL) == 0, "the report of 40 was refused");
    check_sleep(350);
    size_t second = channel_count(ch);
    REQUIRE(yoke_health(u, 10, "cold", NULL) == 0, "the report of 10 was refused");
    check_sleep(350);
    int warm = 0, cold = 0;
    bool seen_cold = false;
    for (size_t i = first; i < channel_count(ch); i++) {
        const Yoke__Plugin__V1__Envelope *e = channel_arrival(ch, i, NULL);
        if (e->payload_case != YOKE__PLUGIN__V1__ENVELOPE__PAYLOAD_HEALTH) {
            continue;
        }
        const Yoke__Plugin__V1__Health *h = e->health;
        bool is_warm = h->grade == 40 && strcmp(h->line, "warming") == 0;
        bool is_cold = h->grade == 10 && strcmp(h->line, "cold") == 0;
        REQUIRE(is_warm || is_cold, "a report carries %u and %s, which the author did not state", h->grade,
                h->line);
        REQUIRE(!(is_warm && seen_cold), "40 was repeated after the author reported 10");
        if (is_cold) {
            seen_cold = true;
            cold++;
        } else if (i < second) {
            warm++;
        }
    }
    /* Each count includes the author's own report. */
    REQUIRE(warm >= 3, "%d beats repeated 40", warm - 1);
    REQUIRE(cold >= 3, "%d beats repeated 10", cold - 1);
done:
    close_all(ch, u);
}

/* Listens at path for a socket of the type given; the descriptor. */
static int listen_at(const char *path, int type) {
    int fd = socket(AF_UNIX, type, 0);
    struct sockaddr_un addr = {.sun_family = AF_UNIX};
    snprintf(addr.sun_path, sizeof addr.sun_path, "%s", path);
    if (bind(fd, (struct sockaddr *)&addr, sizeof addr) != 0) {
        close(fd);
        return -1;
    }
    if (type == SOCK_SEQPACKET && listen(fd, 4) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

/* Waits at most ms for fd to be readable. */
static bool readable(int fd, int ms) {
    struct pollfd p = {.fd = fd, .events = POLLIN};
    return poll(&p, 1, ms) == 1;
}

static void activate(channel *ch, const char *stream,
                     Yoke__Plugin__V1__Control__Activate__Transport transport, const char *address,
                     char id[32]) {
    Yoke__Plugin__V1__Control__Activate a = YOKE__PLUGIN__V1__CONTROL__ACTIVATE__INIT;
    a.stream = (char *)stream;
    a.transport = transport;
    a.address = (char *)address;
    Yoke__Plugin__V1__Control control = YOKE__PLUGIN__V1__CONTROL__INIT;
    control.kind_case = YOKE__PLUGIN__V1__CONTROL__KIND_ACTIVATE;
    control.activate = &a;
    Yoke__Plugin__V1__Envelope e = YOKE__PLUGIN__V1__ENVELOPE__INIT;
    e.payload_case = YOKE__PLUGIN__V1__ENVELOPE__PAYLOAD_CONTROL;
    e.control = &control;
    channel_send(ch, &e, id);
}

/* The transports of case 15 and 16: a packet socket and a datagram socket the case listens on. */
typedef struct transports {
    char ordered[256], framed[256], nothing[256];
    int listener, datagrams, conn;
} transports;

static bool listen_both(transports *t) {
    snprintf(t->ordered, sizeof t->ordered, "%s/ordered.sock", check_dir());
    snprintf(t->framed, sizeof t->framed, "%s/framed.sock", check_dir());
    snprintf(t->nothing, sizeof t->nothing, "%s/nothing.sock", check_dir());
    t->listener = listen_at(t->ordered, SOCK_SEQPACKET);
    t->datagrams = listen_at(t->framed, SOCK_DGRAM);
    t->conn = -1;
    return t->listener >= 0 && t->datagrams >= 0;
}

static void close_both(transports *t) {
    if (t->conn >= 0) {
        close(t->conn);
    }
    if (t->listener >= 0) {
        close(t->listener);
    }
    if (t->datagrams >= 0) {
        close(t->datagrams);
    }
}

// std: yoke-sdk-c:the-plugin-library.15
static void test_an_activation_connects_to_its_transport_and_is_acknowledged(void) {
    channel *ch = NULL;
    yoke_event ev = {0};
    yoke_error err = {0};
    transports t = {.listener = -1, .datagrams = -1, .conn = -1};
    char ordered_id[32], framed_id[32], nothing_id[32];
    yoke_unit *u = open_unit(&ch, NULL);
    REQUIRE(u != NULL, "the unit did not start");
    REQUIRE(listen_both(&t), "the case cannot listen");
    activate(ch, "station.spectra", YOKE__PLUGIN__V1__CONTROL__ACTIVATE__TRANSPORT__TRANSPORT_ORDERED,
             t.ordered, ordered_id);
    activate(ch, "station.preview", YOKE__PLUGIN__V1__CONTROL__ACTIVATE__TRANSPORT__TRANSPORT_FRAMED,
             t.framed, framed_id);
    activate(ch, "station.raw", YOKE__PLUGIN__V1__CONTROL__ACTIVATE__TRANSPORT__TRANSPORT_ORDERED, t.nothing,
             nothing_id);
    REQUIRE(readable(t.listener, 3000), "the library did not connect to the packet socket");
    t.conn = accept(t.listener, NULL, NULL);
    size_t from = 0;
    const Yoke__Plugin__V1__Ack *a = ack_of(ch, ordered_id, &from);
    REQUIRE(a != NULL && a->outcome == YOKE__PLUGIN__V1__ACK__OUTCOME__OUTCOME_DONE,
            "the ordered activation is not acknowledged as done");
    from = 0;
    a = ack_of(ch, framed_id, &from);
    REQUIRE(a != NULL && a->outcome == YOKE__PLUGIN__V1__ACK__OUTCOME__OUTCOME_DONE,
            "the framed activation is not acknowledged as done");
    from = 0;
    a = ack_of(ch, nothing_id, &from);
    REQUIRE(a != NULL && a->outcome == YOKE__PLUGIN__V1__ACK__OUTCOME__OUTCOME_FAILED && a->line[0] != '\0',
            "the activation nothing listens for is not acknowledged as failed, with a line");
    REQUIRE(next_of(u, &ev, YOKE_ACTIVATED), "the first activation was not handed to the author");
    REQUIRE(strcmp(ev.stream, "station.spectra") == 0 && strcmp(ev.transport, "ordered") == 0 &&
                strcmp(ev.address, t.ordered) == 0,
            "the author was handed %s on %s at %s", ev.stream, ev.transport, ev.address);
    yoke_event_clear(&ev);
    REQUIRE(next_of(u, &ev, YOKE_ACTIVATED), "the second activation was not handed to the author");
    REQUIRE(strcmp(ev.stream, "station.preview") == 0 && strcmp(ev.transport, "framed") == 0,
            "the author was handed %s on %s", ev.stream, ev.transport);
    yoke_event_clear(&ev);
    REQUIRE(yoke_next_within(u, &ev, 200) == -1, "the failed activation was handed to the author");
    REQUIRE(yoke_emit(u, "station.raw", "x", 1, &err) == -1 && yoke_is_refusal(&err, "stream.inactive"),
            "the stream whose activation failed is not inactive");
done:
    yoke_event_clear(&ev);
    yoke_error_clear(&err);
    close_both(&t);
    close_all(ch, u);
}

static unsigned long long le64(const unsigned char *b) {
    unsigned long long v = 0;
    for (int i = 7; i >= 0; i--) {
        v = v << 8 | b[i];
    }
    return v;
}

// std: yoke-sdk-c:the-plugin-library.16
static void test_emit_writes_envelopes_and_frames_numbered_from_one(void) {
    channel *ch = NULL;
    yoke_event ev = {0};
    transports t = {.listener = -1, .datagrams = -1, .conn = -1};
    char ordered_id[32], framed_id[32];
    static const char *const payloads[] = {"one", "two", "three"};
    yoke_unit *u = open_unit(&ch, NULL);
    REQUIRE(u != NULL, "the unit did not start");
    REQUIRE(listen_both(&t), "the case cannot listen");
    activate(ch, "station.spectra", YOKE__PLUGIN__V1__CONTROL__ACTIVATE__TRANSPORT__TRANSPORT_ORDERED,
             t.ordered, ordered_id);
    activate(ch, "station.preview", YOKE__PLUGIN__V1__CONTROL__ACTIVATE__TRANSPORT__TRANSPORT_FRAMED,
             t.framed, framed_id);
    REQUIRE(next_of(u, &ev, YOKE_ACTIVATED), "the first activation was not handed over");
    yoke_event_clear(&ev);
    REQUIRE(next_of(u, &ev, YOKE_ACTIVATED), "the second activation was not handed over");
    REQUIRE(readable(t.listener, 3000), "the library did not connect to the packet socket");
    t.conn = accept(t.listener, NULL, NULL);
    for (int i = 0; i < 3; i++) {
        REQUIRE(yoke_emit(u, "station.spectra", payloads[i], strlen(payloads[i]), NULL) == 0,
                "emitting on the ordered stream failed");
        REQUIRE(yoke_emit(u, "station.preview", payloads[i], strlen(payloads[i]), NULL) == 0,
                "emitting on the framed stream failed");
    }
    for (int i = 0; i < 3; i++) {
        unsigned char packet[4096];
        REQUIRE(readable(t.conn, 3000), "packet %d did not arrive", i + 1);
        ssize_t n = recv(t.conn, packet, sizeof packet, 0);
        REQUIRE(n > 0, "packet %d could not be read", i + 1);
        Yoke__Plugin__V1__Envelope *e = yoke__plugin__v1__envelope__unpack(NULL, (size_t)n, packet);
        REQUIRE(e != NULL, "packet %d is not an envelope", i + 1);
        bool good = e->payload_case == YOKE__PLUGIN__V1__ENVELOPE__PAYLOAD_DATA &&
                    e->data->sequence == (unsigned)i + 1 && e->message_id[0] != '\0' &&
                    strcmp(e->session_id, "s-1") == 0 && e->sent_at_unix_nano > 0 &&
                    e->data->payload.len == strlen(payloads[i]) &&
                    memcmp(e->data->payload.data, payloads[i], e->data->payload.len) == 0;
        yoke__plugin__v1__envelope__free_unpacked(e, NULL);
        REQUIRE(good, "packet %d is not data envelope %d with its header and the payload unchanged", i + 1,
                i + 1);

        REQUIRE(readable(t.datagrams, 3000), "datagram %d did not arrive", i + 1);
        n = recv(t.datagrams, packet, sizeof packet, 0);
        REQUIRE(n == 16 + (ssize_t)strlen(payloads[i]), "datagram %d has %zd bytes", i + 1, n);
        REQUIRE(le64(packet) == (unsigned)i + 1, "datagram %d carries the sequence %llu", i + 1,
                le64(packet));
        REQUIRE(le64(packet + 8) > 0, "datagram %d carries no clock", i + 1);
        REQUIRE(memcmp(packet + 16, payloads[i], strlen(payloads[i])) == 0, "datagram %d's payload changed",
                i + 1);
    }
    REQUIRE(channel_await(ch, YOKE__PLUGIN__V1__ENVELOPE__PAYLOAD_DATA, 0, 0) < 0,
            "data reached the Session");
done:
    yoke_event_clear(&ev);
    close_both(&t);
    close_all(ch, u);
}

// std: yoke-sdk-c:the-plugin-library.17
static void test_a_stop_closes_the_transport_and_emit_is_refused_after_it(void) {
    channel *ch = NULL;
    yoke_event ev = {0};
    yoke_error err = {0};
    transports t = {.listener = -1, .datagrams = -1, .conn = -1};
    char ordered_id[32], stop_id[32];
    yoke_unit *u = open_unit(&ch, NULL);
    REQUIRE(u != NULL, "the unit did not start");
    REQUIRE(listen_both(&t), "the case cannot listen");
    activate(ch, "station.spectra", YOKE__PLUGIN__V1__CONTROL__ACTIVATE__TRANSPORT__TRANSPORT_ORDERED,
             t.ordered, ordered_id);
    REQUIRE(next_of(u, &ev, YOKE_ACTIVATED), "the activation was not handed over");
    yoke_event_clear(&ev);
    REQUIRE(readable(t.listener, 3000), "the library did not connect to the packet socket");
    t.conn = accept(t.listener, NULL, NULL);

    Yoke__Plugin__V1__Control__Stop stop = YOKE__PLUGIN__V1__CONTROL__STOP__INIT;
    stop.stream = "station.spectra";
    Yoke__Plugin__V1__Control control = YOKE__PLUGIN__V1__CONTROL__INIT;
    control.kind_case = YOKE__PLUGIN__V1__CONTROL__KIND_STOP;
    control.stop = &stop;
    Yoke__Plugin__V1__Envelope e = YOKE__PLUGIN__V1__ENVELOPE__INIT;
    e.payload_case = YOKE__PLUGIN__V1__ENVELOPE__PAYLOAD_CONTROL;
    e.control = &control;
    channel_send(ch, &e, stop_id);

    REQUIRE(next_of(u, &ev, YOKE_STOPPED), "the stop was not handed to the author");
    REQUIRE(strcmp(ev.stream, "station.spectra") == 0, "the author was handed the stop of %s", ev.stream);
    size_t from = 0;
    const Yoke__Plugin__V1__Ack *a = ack_of(ch, stop_id, &from);
    REQUIRE(a != NULL && a->outcome == YOKE__PLUGIN__V1__ACK__OUTCOME__OUTCOME_DONE,
            "the stop is not acknowledged as done");
    unsigned char b[64];
    REQUIRE(readable(t.conn, 3000) && recv(t.conn, b, sizeof b, 0) == 0,
            "the library did not close its connection");
    REQUIRE(yoke_emit(u, "station.spectra", "x", 1, &err) == -1 && yoke_is_refusal(&err, "stream.inactive"),
            "an emission after the stop was not refused with stream.inactive");
done:
    yoke_event_clear(&ev);
    yoke_error_clear(&err);
    close_both(&t);
    close_all(ch, u);
}

int main(void) {
    static const check_case cases[] = {
        {"yoke-sdk-c:the-plugin-library.01", test_a_declaration_generates_the_manifest},
        {"yoke-sdk-c:the-plugin-library.02", test_nothing_the_model_does_not_have_can_be_declared},
        {"yoke-sdk-c:the-plugin-library.03", test_the_registration_claims_what_the_manifest_declares},
        {"yoke-sdk-c:the-plugin-library.04", test_the_socket_is_bound_before_registering},
        {"yoke-sdk-c:the-plugin-library.05", test_a_refusal_is_surfaced_and_never_retried},
        {"yoke-sdk-c:the-plugin-library.06", test_an_acceptance_with_restrictions_names_what_was_withheld},
        {"yoke-sdk-c:the-plugin-library.07", test_the_session_opens_and_beats_on_the_cores_terms},
        {"yoke-sdk-c:the-plugin-library.08", test_the_end_of_a_session_is_surfaced_and_nothing_reconnects},
        {"yoke-sdk-c:the-plugin-library.09", test_an_orderly_close_is_the_units},
        {"yoke-sdk-c:the-plugin-library.10", test_what_the_core_sends_is_surfaced_and_answers_are_correlated},
        {"yoke-sdk-c:the-plugin-library.11", test_an_occurrence_carries_the_authors_severity_or_is_refused},
        {"yoke-sdk-c:the-plugin-library.12", test_nothing_is_emitted_on_a_stream_not_activated},
        {"yoke-sdk-c:the-plugin-library.13", test_every_family_a_unit_originates_has_an_act},
        {"yoke-sdk-c:the-plugin-library.14", test_a_beat_repeats_the_authors_last_report},
        {"yoke-sdk-c:the-plugin-library.15",
         test_an_activation_connects_to_its_transport_and_is_acknowledged},
        {"yoke-sdk-c:the-plugin-library.16", test_emit_writes_envelopes_and_frames_numbered_from_one},
        {"yoke-sdk-c:the-plugin-library.17", test_a_stop_closes_the_transport_and_emit_is_refused_after_it},
    };
    return check_run(cases, sizeof cases / sizeof *cases);
}
