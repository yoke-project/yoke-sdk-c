/*
 * The base: what the libraries of this project share and nothing else. A concept that exists on one
 * contract only does not live here: a candidate is in the base only if every library of the project
 * would otherwise implement it.
 */
#include "wire.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <grpc/byte_buffer_reader.h>
#include <grpc/grpc_security.h>

#include "yoke/plugin/v1/contract.pb-c.h"

int yoke_plugin_contract(void) { return YOKE__PLUGIN__V1__CONTRACT__CONTRACT_VERSION; }

char *yoke_strdup(const char *s) { return s != NULL ? strdup(s) : NULL; }

void yoke_error_clear(yoke_error *err) {
    if (err == NULL) {
        return;
    }
    free(err->code);
    free(err->message);
    free(err->stage);
    free(err->item);
    free(err->subject.kind);
    free(err->subject.identity);
    memset(err, 0, sizeof *err);
}

bool yoke_is_refusal(const yoke_error *err, const char *code) {
    return err != NULL && err->code != NULL && code != NULL && strcmp(err->code, code) == 0;
}

static char *vformat(const char *format, va_list args) {
    char *out = NULL;
    if (vasprintf(&out, format, args) < 0) {
        return NULL;
    }
    return out;
}

void yoke_error_say(yoke_error *err, const char *format, ...) {
    if (err == NULL) {
        return;
    }
    yoke_error_clear(err);
    va_list args;
    va_start(args, format);
    err->message = vformat(format, args);
    va_end(args);
}

void yoke_error_refuse(yoke_error *err, const char *code, const char *format, ...) {
    if (err == NULL) {
        return;
    }
    yoke_error_clear(err);
    err->code = yoke_strdup(code);
    va_list args;
    va_start(args, format);
    err->message = vformat(format, args);
    va_end(args);
}

void yoke_refusal_of(const Yoke__Plugin__V1__Error *envelope, yoke_error *err) {
    yoke_error_refuse(err, envelope->code, "%s", envelope->message ? envelope->message : "");
    if (err != NULL && envelope->detail_case == YOKE__PLUGIN__V1__ERROR__DETAIL_WITHHELD &&
        envelope->withheld != NULL) {
        err->item = yoke_strdup(envelope->withheld->item);
    }
}

void yoke_env_clear(yoke_env *env) {
    if (env == NULL) {
        return;
    }
    free(env->plugin);
    free(env->unit);
    free(env->socket);
    free(env->bind);
    free(env->token);
    memset(env, 0, sizeof *env);
}

int yoke_environment(yoke_env *env, yoke_getenv getenv, void *context, yoke_error *err) {
    static const char *const required[] = {"YOKE_UNIT", "YOKE_SOCKET", "YOKE_BIND"};
    char missing[64] = "";
    for (size_t i = 0; i < sizeof required / sizeof *required; i++) {
        const char *value = getenv(required[i], context);
        if (value == NULL || value[0] == '\0') {
            if (missing[0] != '\0') {
                strcat(missing, ", ");
            }
            strcat(missing, required[i]);
        }
    }
    if (missing[0] != '\0') {
        yoke_error_say(err, "the environment does not carry %s", missing);
        return -1;
    }
    const char *plugin = getenv("YOKE_PLUGIN", context), *token = getenv("YOKE_TOKEN", context);
    env->plugin = strdup(plugin ? plugin : "");
    env->unit = strdup(getenv("YOKE_UNIT", context));
    env->socket = strdup(getenv("YOKE_SOCKET", context));
    env->bind = strdup(getenv("YOKE_BIND", context));
    env->token = strdup(token ? token : "");
    return 0;
}

grpc_channel *yoke_dial(const char *path) {
    char *target = NULL;
    if (asprintf(&target, "unix:%s", path) < 0) {
        return NULL;
    }
    grpc_channel_credentials *credentials = grpc_insecure_credentials_create();
    grpc_channel *channel = grpc_channel_create(target, credentials, NULL);
    grpc_channel_credentials_release(credentials);
    free(target);
    return channel;
}

grpc_byte_buffer *yoke_pack(const ProtobufCMessage *message) {
    size_t len = protobuf_c_message_get_packed_size(message);
    grpc_slice slice = grpc_slice_malloc(len);
    protobuf_c_message_pack(message, GRPC_SLICE_START_PTR(slice));
    grpc_byte_buffer *buffer = grpc_raw_byte_buffer_create(&slice, 1);
    grpc_slice_unref(slice);
    return buffer;
}

unsigned char *yoke_bytes(grpc_byte_buffer *buffer, size_t *len) {
    grpc_byte_buffer_reader reader;
    if (!grpc_byte_buffer_reader_init(&reader, buffer)) {
        return NULL;
    }
    grpc_slice all = grpc_byte_buffer_reader_readall(&reader);
    grpc_byte_buffer_reader_destroy(&reader);
    *len = GRPC_SLICE_LENGTH(all);
    unsigned char *out = malloc(*len + 1);
    if (out != NULL) {
        memcpy(out, GRPC_SLICE_START_PTR(all), *len);
    }
    grpc_slice_unref(all);
    return out;
}

long long yoke_now(void) {
    struct timespec t;
    clock_gettime(CLOCK_REALTIME, &t);
    return (long long)t.tv_sec * 1000000000 + t.tv_nsec;
}

void yoke_envelopes_init(yoke_envelopes *envelopes, const char *session) {
    envelopes->session = strdup(session ? session : "");
    envelopes->next = 0;
    pthread_mutex_init(&envelopes->mu, NULL);
}

void yoke_envelopes_destroy(yoke_envelopes *envelopes) {
    free(envelopes->session);
    envelopes->session = NULL;
    pthread_mutex_destroy(&envelopes->mu);
}

void yoke_seal(yoke_envelopes *envelopes, Yoke__Plugin__V1__Envelope *e, char id[YOKE_ID_SIZE]) {
    pthread_mutex_lock(&envelopes->mu);
    unsigned long long n = ++envelopes->next;
    pthread_mutex_unlock(&envelopes->mu);
    snprintf(id, YOKE_ID_SIZE, "u-%llu", n);
    e->message_id = id;
    e->session_id = envelopes->session;
    e->sent_at_unix_nano = yoke_now();
}

void yoke_answer_to(yoke_envelopes *envelopes, const char *to, Yoke__Plugin__V1__Envelope *e,
                    char id[YOKE_ID_SIZE]) {
    yoke_seal(envelopes, e, id);
    e->correlation_id = (char *)to;
}
