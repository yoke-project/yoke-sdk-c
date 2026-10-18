#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <openssl/err.h>
#include <openssl/ssl.h>

#include "check.h"
#include "core.h"
#include "tls.h"
#include "yoke/interface.h"

/* A root, and a chain for `localhost` issued by it, made by openssl. */
typedef struct pki {
    char dir[300], chain[320], key[320];
    char *root;
} pki;

static int run(const char *dir, const char *command) {
    char line[2048];
    snprintf(line, sizeof line, "cd %s && %s >/dev/null 2>&1", dir, command);
    return system(line);
}

static bool make_pki(pki *p, const char *name) {
    snprintf(p->dir, sizeof p->dir, "%s/%s", check_dir(), name);
    mkdir(p->dir, 0700);
    const char *ec = "-newkey ec -pkeyopt ec_paramgen_curve:prime256v1 -nodes";
    char command[1024];
    snprintf(command, sizeof command,
             "openssl req -x509 %s -keyout root.key -out root.pem -subj /CN=YokeTestRoot -days 2 "
             "-addext basicConstraints=critical,CA:TRUE -addext keyUsage=critical,keyCertSign",
             ec);
    if (run(p->dir, command) != 0) {
        return false;
    }
    snprintf(command, sizeof command, "openssl req %s -keyout leaf.key -out leaf.csr -subj /CN=localhost",
             ec);
    if (run(p->dir, command) != 0) {
        return false;
    }
    if (run(p->dir, "printf 'subjectAltName=DNS:localhost,IP:127.0.0.1\\nkeyUsage=digitalSignature\\n"
                    "extendedKeyUsage=serverAuth\\n' > leaf.ext") != 0 ||
        run(p->dir,
            "openssl x509 -req -in leaf.csr -CA root.pem -CAkey root.key -CAcreateserial -out leaf.pem "
            "-days 2 -extfile leaf.ext") != 0) {
        return false;
    }
    snprintf(p->chain, sizeof p->chain, "%s/leaf.pem", p->dir);
    snprintf(p->key, sizeof p->key, "%s/leaf.key", p->dir);
    char root[320];
    snprintf(root, sizeof root, "%s/root.pem", p->dir);
    p->root = check_read(root);
    return p->root != NULL;
}

/* TLS on loopback in front of a Core's local socket, limited to one protocol version. */
typedef struct terminator {
    SSL_CTX *ctx;
    int listener;
    char address[64];
    const char *upstream;
    pthread_mutex_t mu;
    int connections, negotiated;
    int version; /* what the last connection negotiated */
    bool h2, client_certificate;
} terminator;

static int select_h2(SSL *ssl, const unsigned char **out, unsigned char *outlen, const unsigned char *in,
                     unsigned inlen, void *arg) {
    (void)ssl;
    (void)arg;
    static const unsigned char h2[] = {2, 'h', '2'};
    if (SSL_select_next_proto((unsigned char **)(void *)out, outlen, h2, sizeof h2, in, inlen) ==
        OPENSSL_NPN_NEGOTIATED) {
        return SSL_TLSEXT_ERR_OK;
    }
    return SSL_TLSEXT_ERR_NOACK;
}

static void *accept_loop(void *arg) {
    terminator *t = arg;
    for (;;) {
        int net = accept(t->listener, NULL, NULL);
        if (net < 0) {
            return NULL;
        }
        pthread_mutex_lock(&t->mu);
        t->connections++;
        pthread_mutex_unlock(&t->mu);
        SSL *ssl = SSL_new(t->ctx);
        SSL_set_fd(ssl, net);
        if (SSL_accept(ssl) != 1) {
            SSL_free(ssl);
            close(net);
            continue;
        }
        const unsigned char *selected = NULL;
        unsigned length = 0;
        SSL_get0_alpn_selected(ssl, &selected, &length);
        pthread_mutex_lock(&t->mu);
        t->negotiated++;
        t->version = SSL_version(ssl);
        t->h2 = length == 2 && memcmp(selected, "h2", 2) == 0;
        X509 *peer = SSL_get_peer_certificate(ssl);
        t->client_certificate = peer != NULL;
        if (peer != NULL) {
            X509_free(peer);
        }
        pthread_mutex_unlock(&t->mu);
        int local = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        struct sockaddr_un addr = {.sun_family = AF_UNIX};
        strcpy(addr.sun_path, t->upstream);
        if (connect(local, (struct sockaddr *)&addr, sizeof addr) != 0) {
            SSL_free(ssl);
            close(net);
            close(local);
            continue;
        }
        yoke_pump_start(ssl, net, local);
    }
}

static terminator *terminate(const pki *p, int version, const char *upstream) {
    terminator *t = calloc(1, sizeof *t);
    t->ctx = SSL_CTX_new(TLS_server_method());
    SSL_CTX_set_min_proto_version(t->ctx, version);
    SSL_CTX_set_max_proto_version(t->ctx, version);
    SSL_CTX_use_certificate_chain_file(t->ctx, p->chain);
    SSL_CTX_use_PrivateKey_file(t->ctx, p->key, SSL_FILETYPE_PEM);
    SSL_CTX_set_alpn_select_cb(t->ctx, select_h2, NULL);
    t->upstream = upstream;
    pthread_mutex_init(&t->mu, NULL);
    t->listener = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    bind(t->listener, (struct sockaddr *)&addr, sizeof addr);
    listen(t->listener, 8);
    socklen_t len = sizeof addr;
    getsockname(t->listener, (struct sockaddr *)&addr, &len);
    snprintf(t->address, sizeof t->address, "127.0.0.1:%d", ntohs(addr.sin_port));
    pthread_t thread;
    pthread_create(&thread, NULL, accept_loop, t);
    pthread_detach(thread);
    return t;
}

static void script_greets(core_conn *c, void *arg) {
    (void)arg;
    core_send(c, core_opening("standing", 1, NULL, 0));
    const Yoke__Interface__V1__ClientFrame *f;
    while (core_next(c, &f, -1) == CORE_FRAME) {
    }
}

static const Yoke__Interface__V1__Request *request_of(const Yoke__Interface__V1__ClientFrame *f) {
    return f->carries_case == YOKE__INTERFACE__V1__CLIENT_FRAME__CARRIES_REQUEST ? f->request : NULL;
}

/* A Core on its own socket under a directory of its own. */
static core *core_in(const char *name, core_script script) {
    static char dirs[8][320];
    static int next;
    char *dir = dirs[next++];
    snprintf(dir, 320, "%s/%s", check_dir(), name);
    mkdir(dir, 0700);
    return core_start(dir, script, NULL);
}

// std: yoke-sdk-c:across-a-routable-address.01
static void test_the_library_dials_tls_13_alone_against_the_roots_its_caller_supplies(void) {
    pki a = {0}, b = {0};
    yoke_error err = {0};
    yoke_attachment *attached = NULL;
    REQUIRE(make_pki(&a, "a") && make_pki(&b, "b"),
            "the case cannot make its certificates (is openssl there?)");
    yoke_attach_options roots = {.roots_pem = a.root, .tls_name = "localhost", .timeout_ms = 10000};

    core *good = core_in("good", script_greets);
    terminator *t13 = terminate(&a, TLS1_3_VERSION, core_socket(good));
    attached = yoke_attach(t13->address, &roots, &err);
    REQUIRE(attached != NULL && yoke_interface_version(attached) == 1, "the first attachment failed: %s",
            err.message ? err.message : "");
    pthread_mutex_lock(&t13->mu);
    REQUIRE(t13->negotiated == 1 && t13->version == TLS1_3_VERSION && t13->h2 && !t13->client_certificate,
            "the Core read TLS %x, h2 %d, a client certificate %d", t13->version, t13->h2,
            t13->client_certificate);
    pthread_mutex_unlock(&t13->mu);
    yoke_detach(attached);
    attached = NULL;

    core *old = core_in("old", script_greets);
    terminator *t12 = terminate(&a, TLS1_2_VERSION, core_socket(old));
    yoke_error_clear(&err);
    REQUIRE(yoke_attach(t12->address, &roots, &err) == NULL, "an attachment was made over TLS 1.2");
    REQUIRE(core_attachments(old) == 0, "something attached over TLS 1.2");

    core *stranger = core_in("stranger", script_greets);
    terminator *tb = terminate(&b, TLS1_3_VERSION, core_socket(stranger));
    yoke_error_clear(&err);
    REQUIRE(yoke_attach(tb->address, &roots, &err) == NULL,
            "an attachment was made to a chain of another root");
    REQUIRE(core_attachments(stranger) == 0, "something attached to a chain of another root");

    core *untouched = core_in("untouched", script_greets);
    terminator *tn = terminate(&a, TLS1_3_VERSION, core_socket(untouched));
    yoke_attach_options empty = {.roots_pem = "", .tls_name = "localhost"};
    yoke_error_clear(&err);
    REQUIRE(yoke_attach(tn->address, &empty, &err) == NULL && err.message != NULL &&
                strstr(err.message, "roots") != NULL,
            "an attachment with no roots was not refused naming them");
    pthread_mutex_lock(&tn->mu);
    REQUIRE(tn->connections == 0, "a connection was dialled with no roots");
    pthread_mutex_unlock(&tn->mu);

    yoke_error_clear(&err);
    REQUIRE(yoke_attach("/tmp/yoke-no-such.sock", &roots, &err) == NULL && err.message != NULL &&
                strstr(err.message, "local socket") != NULL,
            "TLS on a local socket was not refused");
done:
    yoke_error_clear(&err);
    if (attached != NULL) {
        yoke_detach(attached);
    }
}

/* A Core that reads the first frame before it opens: it accepts `alice` with `hunter2x` and the token it
 * answers, and refuses anything else `auth.invalid`. */
static void script_judges(core_conn *c, void *arg) {
    (void)arg;
    const Yoke__Interface__V1__ClientFrame *first;
    if (core_next(c, &first, 5000) != CORE_FRAME) {
        return;
    }
    const Yoke__Interface__V1__Request *r = request_of(first);
    bool accepted = false;
    if (r != NULL && r->operation_case == YOKE__INTERFACE__V1__REQUEST__OPERATION_AUTHENTICATE) {
        const Yoke__Interface__V1__Authenticate *auth = r->authenticate;
        if (auth->credential_case == YOKE__INTERFACE__V1__AUTHENTICATE__CREDENTIAL_PASSWORD) {
            accepted = strcmp(auth->password->account, "alice") == 0 &&
                       strcmp(auth->password->secret, "hunter2x") == 0;
        } else if (auth->credential_case == YOKE__INTERFACE__V1__AUTHENTICATE__CREDENTIAL_TOKEN) {
            accepted = strcmp(auth->token, "tok-alice") == 0;
        }
    }
    char call[32];
    snprintf(call, sizeof call, "%s", first->call);
    core_send(c, core_opening("standing", 1, NULL, 0));
    if (accepted) {
        Yoke__Interface__V1__Authenticated authenticated = YOKE__INTERFACE__V1__AUTHENTICATED__INIT;
        Google__Protobuf__Timestamp expires = GOOGLE__PROTOBUF__TIMESTAMP__INIT;
        expires.seconds = 4000000000LL;
        authenticated.account = (char *)"alice";
        authenticated.token = (char *)"tok-alice";
        authenticated.expires = &expires;
        Yoke__Interface__V1__Response resp = YOKE__INTERFACE__V1__RESPONSE__INIT;
        resp.answer_case = YOKE__INTERFACE__V1__RESPONSE__ANSWER_AUTHENTICATE;
        resp.authenticate = &authenticated;
        Yoke__Interface__V1__CoreFrame f = YOKE__INTERFACE__V1__CORE_FRAME__INIT;
        f.call = call;
        f.carries_case = YOKE__INTERFACE__V1__CORE_FRAME__CARRIES_ANSWER;
        f.answer = &resp;
        core_send(c, &f);
    } else {
        Yoke__Interface__V1__Refusal refusal = YOKE__INTERFACE__V1__REFUSAL__INIT;
        refusal.code = (char *)"auth.invalid";
        refusal.message = (char *)"no";
        core_send(c, core_refusal_frame(call, &refusal));
    }
    const Yoke__Interface__V1__ClientFrame *f;
    while (core_next(c, &f, -1) == CORE_FRAME) {
    }
}

// std: yoke-sdk-c:across-a-routable-address.02
static void test_the_credential_is_the_attachments_first_frame_and_its_answer_is_surfaced(void) {
    pki p = {0};
    yoke_error err = {0};
    yoke_attachment *by_password = NULL, *by_token = NULL, *refused = NULL;
    REQUIRE(make_pki(&p, "p"), "the case cannot make its certificates (is openssl there?)");
    core *k = core_in("judge", script_judges);
    terminator *t = terminate(&p, TLS1_3_VERSION, core_socket(k));

    yoke_attach_options password = {.roots_pem = p.root,
                                    .tls_name = "localhost",
                                    .account = "alice",
                                    .secret = "hunter2x",
                                    .timeout_ms = 10000};
    by_password = yoke_attach(t->address, &password, &err);
    REQUIRE(by_password != NULL && yoke_established(by_password) != NULL &&
                strcmp(yoke_established(by_password)->account, "alice") == 0 &&
                strcmp(yoke_established(by_password)->token, "tok-alice") == 0 &&
                yoke_established(by_password)->expires != NULL &&
                yoke_established(by_password)->expires->seconds == 4000000000LL,
            "the password attachment: %s", err.message ? err.message : "no account was established");
    yoke_detach(by_password);
    by_password = NULL;

    yoke_attach_options token = {
        .roots_pem = p.root, .tls_name = "localhost", .token = "tok-alice", .timeout_ms = 10000};
    by_token = yoke_attach(t->address, &token, &err);
    REQUIRE(by_token != NULL && yoke_established(by_token) != NULL &&
                strcmp(yoke_established(by_token)->account, "alice") == 0,
            "the token attachment: %s", err.message ? err.message : "no account was established");
    yoke_detach(by_token);
    by_token = NULL;

    yoke_attach_options wrong = {.roots_pem = p.root,
                                 .tls_name = "localhost",
                                 .account = "alice",
                                 .secret = "other",
                                 .timeout_ms = 10000};
    yoke_error_clear(&err);
    refused = yoke_attach(t->address, &wrong, &err);
    REQUIRE(refused == NULL && yoke_is_refusal(&err, "auth.invalid"), "the wrong secret was %s",
            err.code ? err.code : "not refused");

    for (int attachment = 1; attachment <= 3; attachment++) {
        const Yoke__Interface__V1__ClientFrame *first = NULL;
        for (size_t n = 0; n < core_frames(k) && first == NULL; n++) {
            int on = 0;
            const Yoke__Interface__V1__ClientFrame *f = core_frame(k, n, &on);
            if (on == attachment) {
                first = f;
            }
        }
        const Yoke__Interface__V1__Request *r = first ? request_of(first) : NULL;
        REQUIRE(r != NULL && r->version == 1 &&
                    r->operation_case == YOKE__INTERFACE__V1__REQUEST__OPERATION_AUTHENTICATE,
                "the first frame of attachment %d was not authenticate stating the version", attachment);
        if (attachment == 1) {
            REQUIRE(r->authenticate->credential_case ==
                            YOKE__INTERFACE__V1__AUTHENTICATE__CREDENTIAL_PASSWORD &&
                        strcmp(r->authenticate->password->secret, "hunter2x") == 0,
                    "the first credential was not the password");
        } else if (attachment == 2) {
            REQUIRE(r->authenticate->credential_case == YOKE__INTERFACE__V1__AUTHENTICATE__CREDENTIAL_TOKEN &&
                        strcmp(r->authenticate->token, "tok-alice") == 0,
                    "the second credential was not the token");
        }
    }
done:
    yoke_error_clear(&err);
    if (by_password != NULL) {
        yoke_detach(by_password);
    }
    if (by_token != NULL) {
        yoke_detach(by_token);
    }
}

int main(void) {
    static const check_case cases[] = {
        {"yoke-sdk-c:across-a-routable-address.01",
         test_the_library_dials_tls_13_alone_against_the_roots_its_caller_supplies},
        {"yoke-sdk-c:across-a-routable-address.02",
         test_the_credential_is_the_attachments_first_frame_and_its_answer_is_surfaced},
    };
    return check_run(cases, sizeof cases / sizeof *cases);
}
