#include "tls.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/x509v3.h>

#include "wire.h"

/* How much either direction holds before it stops reading. */
#define HELD ((size_t)1 << 20)

typedef struct buf {
    unsigned char *data;
    size_t len, cap;
} buf;

static void buf_add(buf *b, const void *bytes, size_t n) {
    if (b->len + n > b->cap) {
        b->cap = (b->len + n) * 2;
        b->data = realloc(b->data, b->cap);
    }
    memcpy(b->data + b->len, bytes, n);
    b->len += n;
}

static void buf_drop(buf *b, size_t n) {
    memmove(b->data, b->data + n, b->len - n);
    b->len -= n;
}

struct yoke_pump {
    SSL *ssl;
    int net, local;
    int stop[2];
    pthread_t thread;
};

static void nonblocking(int fd) { fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK); }

static void *run(void *arg) {
    yoke_pump *p = arg;
    buf out = {0}, in = {0}; /* out: from the local socket, for the connection; in: the other way */
    bool local_open = true;  /* the local side still sends */
    bool want_write = false; /* OpenSSL last asked to write before it could go on */
    bool shut = false;       /* the connection was told the local side is done */
    for (;;) {
        struct pollfd fds[3] = {
            {p->net, (short)(POLLIN | (want_write || out.len > 0 ? POLLOUT : 0)), 0},
            {p->local, (short)((local_open && out.len < HELD ? POLLIN : 0) | (in.len > 0 ? POLLOUT : 0)), 0},
            {p->stop[0], POLLIN, 0}};
        if (poll(fds, 3, -1) < 0 && errno != EINTR) {
            break;
        }
        if (fds[2].revents) {
            break;
        }
        /* What the local side sent, for the connection. */
        if (local_open && out.len < HELD) {
            unsigned char chunk[16384];
            ssize_t n = read(p->local, chunk, sizeof chunk);
            if (n > 0) {
                buf_add(&out, chunk, (size_t)n);
            } else if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
                local_open = false;
            }
        }
        want_write = false;
        while (out.len > 0) {
            int n = SSL_write(p->ssl, out.data, (int)(out.len < (1 << 30) ? out.len : (1 << 30)));
            if (n > 0) {
                buf_drop(&out, (size_t)n);
                continue;
            }
            int e = SSL_get_error(p->ssl, n);
            if (e == SSL_ERROR_WANT_WRITE || e == SSL_ERROR_WANT_READ) {
                want_write = e == SSL_ERROR_WANT_WRITE;
                break;
            }
            goto done; /* the connection failed: the local side sees its end when the pump is let go */
        }
        if (!local_open && out.len == 0 && !shut) {
            shut = true;
            /* The local side is done: say so on the connection, and let the peer end it. */
            SSL_shutdown(p->ssl);
            shutdown(p->net, SHUT_WR);
        }
        /* What the connection brought, for the local side. */
        bool net_ended = false;
        while (in.len < HELD) {
            unsigned char chunk[16384];
            int n = SSL_read(p->ssl, chunk, sizeof chunk);
            if (n > 0) {
                buf_add(&in, chunk, (size_t)n);
                continue;
            }
            int e = SSL_get_error(p->ssl, n);
            if (e == SSL_ERROR_WANT_READ) {
                break;
            }
            if (e == SSL_ERROR_WANT_WRITE) {
                want_write = true;
                break;
            }
            net_ended = true;
            break;
        }
        while (in.len > 0) {
            ssize_t n = write(p->local, in.data, in.len);
            if (n > 0) {
                buf_drop(&in, (size_t)n);
                continue;
            }
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
                break;
            }
            goto done;
        }
        if (net_ended && in.len == 0) {
            /* The connection ended: the local side reads the end. */
            shutdown(p->local, SHUT_WR);
            goto done;
        }
    }
done:
    free(out.data);
    free(in.data);
    return NULL;
}

yoke_pump *yoke_pump_start(SSL *ssl, int net, int local) {
    yoke_pump *p = calloc(1, sizeof *p);
    p->ssl = ssl;
    p->net = net;
    p->local = local;
    if (pipe2(p->stop, O_CLOEXEC) != 0) {
        free(p);
        return NULL;
    }
    nonblocking(net);
    nonblocking(local);
    SSL_set_mode(ssl, SSL_MODE_ENABLE_PARTIAL_WRITE | SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
    pthread_create(&p->thread, NULL, run, p);
    return p;
}

void yoke_pump_free(yoke_pump *p) {
    if (p == NULL) {
        return;
    }
    if (write(p->stop[1], "", 1) < 0) {
        /* the pump is stopped by its pipe closing as well */
    }
    pthread_join(p->thread, NULL);
    SSL_free(p->ssl);
    close(p->net);
    close(p->local);
    close(p->stop[0]);
    close(p->stop[1]);
    free(p);
}

static char *said(void) {
    unsigned long e = ERR_get_error();
    if (e == 0) {
        return strdup("no further reason");
    }
    char text[256];
    ERR_error_string_n(e, text, sizeof text);
    ERR_clear_error();
    return strdup(text);
}

/* host and port from `host:port` or `[host]:port`; 0 when the address is one. */
static int split(const char *address, char **host, char **port) {
    if (address[0] == '[') {
        const char *close = strchr(address, ']');
        if (close == NULL || close[1] != ':' || close[2] == '\0') {
            return -1;
        }
        *host = strndup(address + 1, (size_t)(close - address - 1));
        *port = strdup(close + 2);
        return 0;
    }
    const char *colon = strrchr(address, ':');
    if (colon == NULL || colon == address || colon[1] == '\0') {
        return -1;
    }
    *host = strndup(address, (size_t)(colon - address));
    *port = strdup(colon + 1);
    return 0;
}

static int dial(const char *host, const char *port, int wait_ms, char **why) {
    struct addrinfo hints = {0}, *found = NULL;
    hints.ai_socktype = SOCK_STREAM;
    int e = getaddrinfo(host, port, &hints, &found);
    if (e != 0) {
        if (asprintf(why, "it cannot be resolved: %s", gai_strerror(e)) < 0) {
            *why = NULL;
        }
        return -1;
    }
    *why = strdup("it resolves to nothing");
    int fd = -1;
    for (struct addrinfo *a = found; a != NULL && fd < 0; a = a->ai_next) {
        fd = socket(a->ai_family, a->ai_socktype | SOCK_CLOEXEC, a->ai_protocol);
        if (fd < 0) {
            free(*why);
            *why = strdup(strerror(errno));
            continue;
        }
        struct timeval tv = {wait_ms / 1000, (wait_ms % 1000) * 1000};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
        if (connect(fd, a->ai_addr, a->ai_addrlen) != 0) {
            free(*why);
            *why = strdup(strerror(errno));
            close(fd);
            fd = -1;
        }
    }
    freeaddrinfo(found);
    return fd;
}

yoke_pump *yoke_tls_connect(const char *address, const char *roots_pem, const char *name, int wait_ms,
                            int *fd, yoke_error *err) {
    char *host = NULL, *port = NULL, *why = NULL;
    if (split(address, &host, &port) != 0) {
        yoke_error_say(err, "the address %s is not host:port", address);
        return NULL;
    }
    const char *expected = name != NULL && name[0] != '\0' ? name : host;
    yoke_pump *result = NULL;
    SSL_CTX *ctx = SSL_CTX_new(TLS_client_method());
    SSL *ssl = NULL;
    int net = -1;
    if (ctx == NULL) {
        why = said();
        yoke_error_say(err, "TLS cannot be set up: %s", why);
        goto out;
    }
    /* TLS 1.3, and nothing below it. */
    SSL_CTX_set_min_proto_version(ctx, TLS1_3_VERSION);
    SSL_CTX_set_max_proto_version(ctx, TLS1_3_VERSION);
    /* The roots the caller supplied, and no others: the system's are never loaded. */
    BIO *bio = BIO_new_mem_buf(roots_pem, (int)strlen(roots_pem));
    int loaded = 0;
    X509 *cert;
    while ((cert = PEM_read_bio_X509(bio, NULL, NULL, NULL)) != NULL) {
        X509_STORE_add_cert(SSL_CTX_get_cert_store(ctx), cert);
        X509_free(cert);
        loaded++;
    }
    BIO_free(bio);
    ERR_clear_error();
    if (loaded == 0) {
        yoke_error_say(err,
                       "the roots hold no certificate, and the Core's chain is verified only against the "
                       "roots its caller supplies");
        goto out;
    }
    SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, NULL);
    static const unsigned char h2[] = {2, 'h', '2'};
    SSL_CTX_set_alpn_protos(ctx, h2, sizeof h2);

    net = dial(host, port, wait_ms, &why);
    if (net < 0) {
        yoke_error_say(err, "the address %s cannot be reached: %s", address, why);
        goto out;
    }
    ssl = SSL_new(ctx);
    if (ssl == NULL) {
        free(why);
        why = said();
        goto handshake;
    }
    X509_VERIFY_PARAM *param = SSL_get0_param(ssl);
    struct in6_addr ip6;
    struct in_addr ip4;
    if (inet_pton(AF_INET, expected, &ip4) == 1 || inet_pton(AF_INET6, expected, &ip6) == 1) {
        X509_VERIFY_PARAM_set1_ip_asc(param, expected);
    } else {
        SSL_set_tlsext_host_name(ssl, expected);
        SSL_set1_host(ssl, expected);
    }
    SSL_set_fd(ssl, net);
    if (SSL_connect(ssl) != 1) {
        long verified = SSL_get_verify_result(ssl);
        free(why);
        why = verified != X509_V_OK ? strdup(X509_verify_cert_error_string(verified)) : said();
        goto handshake;
    }
    const unsigned char *selected = NULL;
    unsigned int length = 0;
    SSL_get0_alpn_selected(ssl, &selected, &length);
    if (length != 2 || memcmp(selected, "h2", 2) != 0) {
        free(why);
        why = strdup("the Core did not select the protocol h2");
        goto handshake;
    }
    /* The handshake is bounded by wait_ms; what follows is not. */
    struct timeval none = {0, 0};
    setsockopt(net, SOL_SOCKET, SO_RCVTIMEO, &none, sizeof none);
    setsockopt(net, SOL_SOCKET, SO_SNDTIMEO, &none, sizeof none);
    int pair[2];
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair) != 0) {
        free(why);
        why = strdup(strerror(errno));
        goto handshake;
    }
    result = yoke_pump_start(ssl, net, pair[0]);
    if (result == NULL) {
        close(pair[0]);
        close(pair[1]);
        free(why);
        why = strdup("the pump cannot be made");
        goto handshake;
    }
    *fd = pair[1];
    ssl = NULL;
    net = -1;
    goto out;
handshake:
    yoke_error_say(err, "the TLS handshake with %s failed: %s", address, why ? why : "unknown");
out:
    if (ssl != NULL) {
        SSL_free(ssl);
    }
    if (net >= 0) {
        close(net);
    }
    SSL_CTX_free(ctx);
    free(why);
    free(host);
    free(port);
    return result;
}
