/*
 * A plugin channel the cases stand in for the Core with: it answers a registration on the terms a case
 * sets, keeps everything that arrives on the Session with the moment it arrived, and sends what a case
 * hands it.
 */
#ifndef YOKE_CHANNEL_H
#define YOKE_CHANNEL_H

#include <stdbool.h>
#include <stddef.h>

#include "yoke/plugin/v1/register.pb-c.h"
#include "yoke/plugin/v1/session.pb-c.h"

/* How the channel answers a registration. A zeroed field takes the default named beside it. */
typedef struct channel_terms {
    Yoke__Plugin__V1__RegisterResponse__Outcome outcome; /* accepted */
    Yoke__Plugin__V1__Stage stage;
    const char *code;
    const char *message;
    const char *session; /* `s-1` */
    Yoke__Plugin__V1__Surface *granted;
    Yoke__Plugin__V1__Surface *withheld;
    int interval_ms; /* 0: no heartbeat terms */
} channel_terms;

typedef struct channel channel;

/* Listens in dir, at `plugin.sock`, and expects the unit to bind `unit.sock` beside it. */
channel *channel_start(const char *dir, const channel_terms *terms);

/* Stops listening, ending any call still open. */
void channel_stop(channel *ch);

/* The path the channel listens at, and the one a unit is expected to bind. */
const char *channel_socket(const channel *ch);
const char *channel_bind(const channel *ch);

/* A getenv for yoke_start_with: the five variables, pointing at this channel. */
const char *channel_getenv(const char *name, void *ch);

/* How many registrations, and how many Sessions, arrived. */
int channel_registrations(channel *ch);
int channel_sessions(channel *ch);

/* The first registration that arrived, or NULL; and whether the unit's socket was there when it did. */
const Yoke__Plugin__V1__RegisterRequest *channel_request(channel *ch);
bool channel_bound_at_registration(channel *ch);

/* How many envelopes the Session brought; the i-th, and when it arrived in check_ms() time. */
size_t channel_count(channel *ch);
const Yoke__Plugin__V1__Envelope *channel_arrival(channel *ch, size_t i, long long *at);

/*
 * Waits for an envelope whose payload is the case given, at index from or later; its index, or -1 if
 * none arrived within timeout_ms.
 */
long channel_await(channel *ch, Yoke__Plugin__V1__Envelope__PayloadCase payload, size_t from, int timeout_ms);

/* Sends e to the unit on the Session, sealing it; its message identity is written into id. */
void channel_send(channel *ch, Yoke__Plugin__V1__Envelope *e, char id[32]);

/* Ends the Session's stream from the channel's side. */
void channel_end(channel *ch);

/* Whether the unit ended its half of the Session's stream, waiting at most timeout_ms. */
bool channel_await_half_close(channel *ch, int timeout_ms);

#endif
