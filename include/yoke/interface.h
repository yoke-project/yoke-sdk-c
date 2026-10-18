/*
 * The library an interface client is written with: the interface contract, on its typed projection, over
 * the base.
 *
 * A client attaches to a channel and is given its opening: what the channel may address, the standing
 * subscription, and the contract's version. Every operation of the contract's union is one function, all
 * on the one connection, each answered on its own; a refusal is an error carrying its code and what it
 * names. Nothing is retried and nothing reconnects, and nothing is sent the author did not ask for: a
 * confirmation is the author's statement of what it has processed, so the library never makes one.
 *
 * A channel at a routable address is reached over TLS 1.3 against the roots the caller supplies, and
 * establishes its caller before anything else: the credential is the attachment's first frame, and the
 * session token its answer carries is the caller's to present to a later attachment.
 *
 * What the Core answers is the contract's own message, which the caller frees with the function named
 * for it; a function that can fail returns 0 when it succeeded and -1 when it did not, and fills the
 * error it is handed. This header names nothing of the administrative contract.
 */
#ifndef YOKE_INTERFACE_H
#define YOKE_INTERFACE_H

#include <stddef.h>
#include <stdint.h>

#include "yoke/base.h"
#include "yoke/interface/v1/interface.pb-c.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The version of the interface contract this library covers, stated on every request. */
int yoke_interface_contract(void);

/*
 * A channel's address, which the caller frees: in the service form, under its runtime directory; in an
 * application instance, derived from the instance's name under the account's runtime directory; and for a
 * managed interface, the one the Core handed it. A name that could choose where the socket is is refused.
 */
int yoke_interface_service(char **address, const char *runtime_dir, const char *channel, yoke_error *err);
int yoke_interface_application(char **address, const char *name, const char *channel, yoke_getenv getenv,
                               void *context, yoke_error *err);
int yoke_interface_managed(char **address, yoke_getenv getenv, void *context, yoke_error *err);

/* How an attachment reaches its channel, and what it presents there. A zeroed field takes its default. */
typedef struct yoke_attach_options {
    /* Non-NULL reaches the channel over TLS 1.3 and nothing below it, verifying the Core's chain against
     * these roots, in PEM, and no others, for tls_name — the address's host where it is NULL. */
    const char *roots_pem;
    const char *tls_name;
    /* An account and its secret, for the host to judge, as the attachment's first frame... */
    const char *account;
    const char *secret;
    /* ...or a session token an earlier authentication on the same channel issued. */
    const char *token;
    /* How long a call waits for the Core, in milliseconds; 0 waits for the answer. */
    int timeout_ms;
} yoke_attach_options;

typedef struct yoke_attachment yoke_attachment;
typedef struct yoke_subscription yoke_subscription;
typedef struct yoke_delivery yoke_delivery;

/*
 * Attaches to the channel at address — a path is a local socket, anything else host:port — and waits for
 * its opening. A Core that speaks another version of the contract is refused before any operation, and
 * the connection let go. A credential among the options is sent before anything else, and attaching waits
 * for its answer too.
 */
yoke_attachment *yoke_attach(const char *address, const yoke_attach_options *options, yoke_error *err);

/*
 * Ends the attachment in order and waits for the Core to end it, leaving it to be read: a call or a wait
 * on it answers that it ended. Whoever reads a subscription or a delivery on another thread is let go by
 * it, and frees what it holds before the attachment is detached.
 */
void yoke_hangup(yoke_attachment *a);

/*
 * Ends the attachment in order, if that was not done, then frees it: it tells the Core the client is done, so
 * the Core reads a client that closed rather than one that went away, and lets the connection go once the
 * Core has ended it. Every subscription and delivery of the attachment is freed first.
 */
void yoke_detach(yoke_attachment *a);

/* What the channel was given when it attached; the contract's version the Core speaks. */
const Yoke__Interface__V1__Snapshot *yoke_picture(const yoke_attachment *a);
int yoke_interface_version(const yoke_attachment *a);

/* The subscription the attachment holds from the start, or NULL; it is the attachment's, and ends with it. */
yoke_subscription *yoke_standing(yoke_attachment *a);

/* The answer to the credential the attachment presented, or NULL where it presented none. */
const Yoke__Interface__V1__Authenticated *yoke_established(const yoke_attachment *a);

/*
 * The operations. Each sends one request stating the contract's version, and waits for its answer: the
 * Response, in which the answer of the operation is the member of its name. yoke_response_free releases it.
 */
int yoke_authenticate(yoke_attachment *a, const char *account, const char *secret,
                      Yoke__Interface__V1__Response **answer, yoke_error *err);
int yoke_authenticate_token(yoke_attachment *a, const char *token, Yoke__Interface__V1__Response **answer,
                            yoke_error *err);
int yoke_read(yoke_attachment *a, const char *kind, const char *identity,
              Yoke__Interface__V1__Response **answer, yoke_error *err);
/* States that the author has processed a subscription up to a sequence. */
int yoke_confirm(yoke_attachment *a, const char *subscription, uint64_t sequence,
                 Yoke__Interface__V1__Response **answer, yoke_error *err);
int yoke_command(yoke_attachment *a, const char *unit, const char *type, const void *payload, size_t len,
                 Yoke__Interface__V1__Response **answer, yoke_error *err);
int yoke_ask(yoke_attachment *a, const char *unit, const char *type, const void *payload, size_t len,
             Yoke__Interface__V1__Response **answer, yoke_error *err);
int yoke_start_stream(yoke_attachment *a, const char *unit, const char *stream,
                      Yoke__Interface__V1__Response **answer, yoke_error *err);
int yoke_stop_stream(yoke_attachment *a, const char *unit, const char *stream,
                     Yoke__Interface__V1__Response **answer, yoke_error *err);
int yoke_reclaim(yoke_attachment *a, Yoke__Interface__V1__Response **answer, yoke_error *err);
void yoke_response_free(Yoke__Interface__V1__Response *answer);

/*
 * Opens a subscription narrowed by a filter, and returns once its snapshot or its refusal arrives. The
 * snapshot is the first delivery.
 */
yoke_subscription *yoke_subscribe(yoke_attachment *a, const Yoke__Interface__V1__Filter *filter,
                                  yoke_error *err);
/* What a confirmation names. */
const char *yoke_subscription_id(const yoke_subscription *s);

/* What waiting for a delivery came to. */
#define YOKE_WAIT_GOT 0       /* one arrived */
#define YOKE_WAIT_COMPLETE 1  /* the Core completed it: nothing more comes */
#define YOKE_WAIT_TIMEOUT 2   /* the time given ran out */
#define YOKE_WAIT_FAILED (-1) /* the attachment ended, or the Core refused; err says which */

/*
 * The next delivery, in order — a snapshot, then events and overflows — waiting at most timeout_ms (-1: as
 * long as it takes). yoke_subscribed_free releases it.
 */
int yoke_subscription_next(yoke_subscription *s, Yoke__Interface__V1__Subscribed **out, int timeout_ms,
                           yoke_error *err);
void yoke_subscribed_free(Yoke__Interface__V1__Subscribed *delivery);

/* Ends the subscription on the wire, and frees it. The standing subscription is the attachment's. */
void yoke_subscription_close(yoke_subscription *s);

/* One message of a stream as a delivery carries it. yoke_frame_clear releases it. */
typedef struct yoke_frame {
    uint64_t sequence;
    uint64_t sent_at; /* the producer's clock, in nanoseconds */
    unsigned char *payload;
    size_t payload_len;
} yoke_frame;
void yoke_frame_clear(yoke_frame *frame);

/*
 * Asks for a copy of a unit's stream, and opens it where the answer says it arrives: a per-subscriber
 * socket the Core listens on, or the attachment's own connection.
 */
yoke_delivery *yoke_subscribe_stream(yoke_attachment *a, const char *unit, const char *stream,
                                     yoke_error *err);
const char *yoke_delivery_id(const yoke_delivery *d);
/* Whether the stream was flowing when the subscription was answered. */
int yoke_delivery_flowing(const yoke_delivery *d);

/* The next frame, in order, with the results of yoke_subscription_next. */
int yoke_delivery_next(yoke_delivery *d, yoke_frame *frame, int timeout_ms, yoke_error *err);

/* Ends the delivery — the Core is told — and frees it. */
int yoke_delivery_release(yoke_delivery *d, yoke_error *err);

#ifdef __cplusplus
}
#endif

#endif
