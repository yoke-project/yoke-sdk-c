/*
 * TLS 1.3 under the interface library, done here rather than by gRPC: the gRPC this family builds on
 * cannot hold a connection to TLS 1.3 alone, and a channel at a routable address takes nothing below it.
 * The handshake is OpenSSL's; once it is done, a pump carries the plaintext between the TLS connection
 * and one end of a socket pair, and gRPC speaks HTTP/2 on the other end as on any connected socket. Never
 * installed.
 */
#ifndef YOKE_INTERFACE_TLS_H
#define YOKE_INTERFACE_TLS_H

#include <openssl/ssl.h>

#include "yoke/base.h"

/* Carries bytes both ways between a TLS connection and a local socket, on a thread of its own, until
 * either side ends; then ends the other. */
typedef struct yoke_pump yoke_pump;

/* Takes ssl, its connected socket and the local socket; all three are the pump's from here. */
yoke_pump *yoke_pump_start(SSL *ssl, int net, int local);

/* Stops the pump and closes what it holds. */
void yoke_pump_free(yoke_pump *p);

/*
 * A TLS 1.3 connection to host:port: the Core's chain verified against roots, in PEM, and no others, for
 * name — host where it is NULL — with ALPN `h2` and no client certificate. Returns the pump carrying it,
 * and sets *fd to the socket gRPC is to speak on; NULL, with err filled, naming what failed.
 */
yoke_pump *yoke_tls_connect(const char *address, const char *roots_pem, const char *name, int wait_ms,
                            int *fd, yoke_error *err);

#endif
