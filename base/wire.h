/*
 * The base's half that is not an author's: connecting to a socket, a message in and out of gRPC, the
 * envelope and its correlation, and an error filled in. Shared by this project's libraries; never
 * installed.
 */
#ifndef YOKE_WIRE_H
#define YOKE_WIRE_H

#include <pthread.h>
#include <stdarg.h>
#include <stddef.h>

#include <grpc/grpc.h>
#include <protobuf-c/protobuf-c.h>

#include "yoke/base.h"
#include "yoke/plugin/v1/session.pb-c.h"

/* Connects to a Unix socket. */
grpc_channel *yoke_dial(const char *path);

/* A message as gRPC carries it. */
grpc_byte_buffer *yoke_pack(const ProtobufCMessage *message);

/* The bytes gRPC delivered, in a buffer the caller frees. */
unsigned char *yoke_bytes(grpc_byte_buffer *buffer, size_t *len);

/* A string of the caller's, copied; NULL stays NULL. */
char *yoke_strdup(const char *s);

/* Fills err, if there is one, with a failure that is not a refusal. */
void yoke_error_say(yoke_error *err, const char *format, ...) __attribute__((format(printf, 2, 3)));

/* Fills err, if there is one, with a refusal: its code, and a message. */
void yoke_error_refuse(yoke_error *err, const char *code, const char *format, ...)
    __attribute__((format(printf, 3, 4)));

/* Fills err, if there is one, with the refusal an error envelope carries. */
void yoke_refusal_of(const Yoke__Plugin__V1__Error *envelope, yoke_error *err);

/* How long an identity a party's envelope is given may be, its terminator included. */
#define YOKE_ID_SIZE 32

/*
 * Fills the header of every envelope one party sends in one Session: a message identity no other of
 * its envelopes has, the Session's identity, and the sender's clock.
 */
typedef struct yoke_envelopes {
    char *session;
    unsigned long long next;
    pthread_mutex_t mu;
} yoke_envelopes;

/* Numbers the envelopes of one Session. */
void yoke_envelopes_init(yoke_envelopes *envelopes, const char *session);

/* Releases what the numbering holds. */
void yoke_envelopes_destroy(yoke_envelopes *envelopes);

/* Fills e's header; the identity is written into id, which e then points at. */
void yoke_seal(yoke_envelopes *envelopes, Yoke__Plugin__V1__Envelope *e, char id[YOKE_ID_SIZE]);

/* Fills e's header as an answer to the message identified by to. */
void yoke_answer_to(yoke_envelopes *envelopes, const char *to, Yoke__Plugin__V1__Envelope *e,
                    char id[YOKE_ID_SIZE]);

/* The sender's clock, in nanoseconds since the epoch. */
long long yoke_now(void);

#endif
