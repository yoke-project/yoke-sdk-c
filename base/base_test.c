#define _GNU_SOURCE
#include <regex.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "check.h"
#include "wire.h"

/* The base's sources, which case 1 reads. */
static const char *const sources[] = {SOURCE_DIR "/base/base.c", SOURCE_DIR "/base/wire.h",
                                      SOURCE_DIR "/include/yoke/base.h"};

/*
 * What belongs to one contract among the definitions, which the base may not refer to. A generated
 * name is the contract's prefix and then the message's, in whichever case; a payload's case names the
 * message it carries.
 */
static bool forbidden(const char *name) {
    static const char *const words[] = {"register", "session", "surface", "heartbeat", "stage", "control",
                                        "query",    "ack",     "data",    "health",    "event"};
    for (size_t i = 0; i < sizeof words / sizeof *words; i++) {
        char payload[64];
        snprintf(payload, sizeof payload, "envelope__payload_%s", words[i]);
        if (strncasecmp(name, words[i], strlen(words[i])) == 0 ||
            strncasecmp(name, payload, strlen(payload)) == 0) {
            return true;
        }
    }
    return false;
}

// std: yoke-sdk-c:the-base.01
static void test_the_base_holds_nothing_of_one_contract(void) {
    regex_t definition, include;
    regcomp(&definition, "yoke__plugin__v1__([a-z0-9_]+)", REG_EXTENDED | REG_ICASE);
    regcomp(&include, "#include +[\"<]([^\">]+)[\">]", REG_EXTENDED);
    int referred = 0;
    for (size_t i = 0; i < sizeof sources / sizeof *sources; i++) {
        char *text = check_read(sources[i]);
        REQUIRE(text != NULL, "%s cannot be read", sources[i]);
        regmatch_t m[2];
        for (const char *at = text; regexec(&definition, at, 2, m, 0) == 0; at += m[0].rm_eo) {
            char name[128] = {0};
            int len = (int)(m[1].rm_eo - m[1].rm_so);
            snprintf(name, sizeof name, "%.*s", len, at + m[1].rm_so);
            if (forbidden(name)) {
                char *copy = strdup(name);
                free(text);
                check_fail("%s refers to %s, which belongs to one contract", sources[i], copy);
                free(copy);
                goto done;
            }
            referred++;
        }
        for (const char *at = text; regexec(&include, at, 2, m, 0) == 0; at += m[0].rm_eo) {
            int len = (int)(m[1].rm_eo - m[1].rm_so);
            const char *header = at + m[1].rm_so;
            if (strncmp(header, "yoke/", 5) == 0 && strncmp(header, "yoke/base.h", (size_t)len) != 0 &&
                strncmp(header, "yoke/plugin/v1/", 15) != 0) {
                char *copy = strndup(header, (size_t)len);
                free(text);
                check_fail("%s includes %s, a header of a library of this project", sources[i], copy);
                free(copy);
                goto done;
            }
        }
        free(text);
    }
    REQUIRE(referred > 0, "the base refers to nothing of the definitions, so nothing was read");
done:
    regfree(&definition);
    regfree(&include);
}

// std: yoke-sdk-c:the-base.02
static void test_a_refusal_carries_its_code_and_envelopes_are_correlated(void) {
    yoke_error err = {0};
    yoke_envelopes envelopes;
    yoke_envelopes_init(&envelopes, "s-7");
    Yoke__Plugin__V1__Error refusal = YOKE__PLUGIN__V1__ERROR__INIT;
    refusal.code = "session.correlation.unknown";
    refusal.message = "nothing was sent with that identity";
    yoke_refusal_of(&refusal, &err);
    REQUIRE(yoke_is_refusal(&err, "session.correlation.unknown"),
            "the error is not a refusal with that code: %s", err.code ? err.code : "(none)");
    REQUIRE(strcmp(err.message, refusal.message) == 0, "the refusal's message is %s", err.message);

    Yoke__Plugin__V1__Envelope e[3] = {YOKE__PLUGIN__V1__ENVELOPE__INIT, YOKE__PLUGIN__V1__ENVELOPE__INIT,
                                       YOKE__PLUGIN__V1__ENVELOPE__INIT};
    char ids[3][YOKE_ID_SIZE];
    yoke_seal(&envelopes, &e[0], ids[0]);
    yoke_seal(&envelopes, &e[1], ids[1]);
    yoke_answer_to(&envelopes, e[0].message_id, &e[2], ids[2]);
    for (int i = 0; i < 3; i++) {
        REQUIRE(strcmp(e[i].session_id, "s-7") == 0, "envelope %d carries the Session %s", i,
                e[i].session_id);
        REQUIRE(e[i].message_id[0] != '\0', "envelope %d has no message identity", i);
        REQUIRE(e[i].sent_at_unix_nano > 0, "envelope %d carries no clock", i);
        for (int j = 0; j < i; j++) {
            REQUIRE(strcmp(e[i].message_id, e[j].message_id) != 0, "envelopes %d and %d share %s", j, i,
                    e[i].message_id);
        }
    }
    REQUIRE(strcmp(e[2].correlation_id, e[0].message_id) == 0, "the answer is correlated to %s, not %s",
            e[2].correlation_id, e[0].message_id);
    REQUIRE(strcmp(e[2].correlation_id, e[2].message_id) != 0, "the answer is correlated to itself");
done:
    yoke_error_clear(&err);
    yoke_envelopes_destroy(&envelopes);
}

static const char *five(const char *name, void *missing) {
    if (missing != NULL && strcmp(name, missing) == 0) {
        return NULL;
    }
    static const char *const values[][2] = {{"YOKE_PLUGIN", "com.yoke.station.acquire"},
                                            {"YOKE_UNIT", "acquire"},
                                            {"YOKE_SOCKET", "/run/yoke/i/plugin.sock"},
                                            {"YOKE_BIND", "/run/yoke/i/units/acquire.sock"},
                                            {"YOKE_TOKEN", "t-0123"}};
    for (size_t i = 0; i < sizeof values / sizeof *values; i++) {
        if (strcmp(name, values[i][0]) == 0) {
            return values[i][1];
        }
    }
    return NULL;
}

// std: yoke-sdk-c:the-base.03
static void test_the_addresses_come_from_the_environment(void) {
    yoke_env env = {0}, without = {0};
    yoke_error err = {0};
    REQUIRE(yoke_environment(&env, five, NULL, &err) == 0, "the environment was not read: %s", err.message);
    REQUIRE(strcmp(env.plugin, "com.yoke.station.acquire") == 0, "the plugin is %s", env.plugin);
    REQUIRE(strcmp(env.unit, "acquire") == 0, "the unit is %s", env.unit);
    REQUIRE(strcmp(env.socket, "/run/yoke/i/plugin.sock") == 0, "the channel is %s", env.socket);
    REQUIRE(strcmp(env.bind, "/run/yoke/i/units/acquire.sock") == 0, "the path to bind is %s", env.bind);
    REQUIRE(strcmp(env.token, "t-0123") == 0, "the token is %s", env.token);

    REQUIRE(yoke_environment(&without, five, "YOKE_SOCKET", &err) == -1,
            "an environment without YOKE_SOCKET was read");
    REQUIRE(err.message != NULL && strstr(err.message, "YOKE_SOCKET") != NULL,
            "the error does not name YOKE_SOCKET: %s", err.message ? err.message : "(none)");
    REQUIRE(without.socket == NULL, "a path was supplied for the channel: %s", without.socket);
done:
    yoke_env_clear(&env);
    yoke_env_clear(&without);
    yoke_error_clear(&err);
}

int main(void) {
    static const check_case cases[] = {
        {"yoke-sdk-c:the-base.01", test_the_base_holds_nothing_of_one_contract},
        {"yoke-sdk-c:the-base.02", test_a_refusal_carries_its_code_and_envelopes_are_correlated},
        {"yoke-sdk-c:the-base.03", test_the_addresses_come_from_the_environment},
    };
    return check_run(cases, sizeof cases / sizeof *cases);
}
