/*
 * An interface Core the cases stand in for the real one with: it serves the channel `panel` at a socket
 * in a directory, runs a script for every attachment, keeps every frame a client sent, and says how each
 * attachment's requests ended. A script runs on a thread of its own, and reads and writes through the
 * connection it is handed.
 */
#ifndef YOKE_CORE_H
#define YOKE_CORE_H

#include <stdbool.h>
#include <stddef.h>

#include "yoke/interface/v1/interface.pb-c.h"

typedef struct core core;
typedef struct core_conn core_conn;

/* A script, run once for each attachment; it ends when it returns. */
typedef void (*core_script)(core_conn *c, void *arg);

/* Serves `panel.sock` in dir, running script for every attachment. */
core *core_start(const char *dir, core_script script, void *arg);

/* Stops serving, ending every attachment; waits for the scripts. */
void core_stop(core *c);

/* The socket the Core serves at. */
const char *core_socket(const core *c);

/* How many attachments came. */
int core_attachments(core *c);

/* How many attachments' requests ended, and how the i-th did: "closed" or "went away". */
size_t core_ends(core *c);
const char *core_end(core *c, size_t i);

/* How many frames the clients sent; the i-th, and the attachment it came on, counted from 1. */
size_t core_frames(core *c);
const Yoke__Interface__V1__ClientFrame *core_frame(core *c, size_t i, int *attachment);

/* Waits for the i-th frame to have arrived, at most timeout_ms; whether it did. */
bool core_await_frames(core *c, size_t count, int timeout_ms);

/* In a script: the attachment's number, from 1. */
int core_conn_number(const core_conn *c);

/*
 * In a script: the next frame of the client's, in a buffer the connection owns until the next call.
 * CORE_FRAME: one arrived. CORE_CLOSED: the client ended its requests. CORE_TIMEOUT: none within timeout_ms.
 * CORE_GONE: the attachment is over.
 */
#define CORE_FRAME 0
#define CORE_CLOSED 1
#define CORE_TIMEOUT 2
#define CORE_GONE 3
int core_next(core_conn *c, const Yoke__Interface__V1__ClientFrame **frame, int timeout_ms);

/* In a script: sends a frame to the client. */
void core_send(core_conn *c, const Yoke__Interface__V1__CoreFrame *frame);

/* In a script: ends the attachment with status OK. */
void core_finish(core_conn *c);

/* In a script: ends the attachment with a refusal, in the status the contract states. */
void core_refuse(core_conn *c, const char *code, const char *message);

/* Frames a script says, made on the heap-free stack; each is valid until the next call. */
const Yoke__Interface__V1__CoreFrame *core_opening(const char *subscription, unsigned version,
                                                   Yoke__Interface__V1__Record **records, size_t n);
const Yoke__Interface__V1__CoreFrame *core_refusal_frame(const char *call,
                                                         const Yoke__Interface__V1__Refusal *r);

#endif
