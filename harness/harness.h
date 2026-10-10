/*
 * The C plugin library's harness: the thinnest translation between the suite's directives and the
 * library. It turns a directive into a library call and what the library surfaces into an observation,
 * reports a refusal as its code and a verb it does not know as unrecognised, and judges nothing: what a
 * case requires lives in the suite. It speaks no wire of its own.
 */
#ifndef YOKE_HARNESS_H
#define YOKE_HARNESS_H

#include "yoke/plugin.h"

/* What the harness declares: one object of every kind, a stream on each transport, each governed. */
const yoke_declaration *harness_declaration(void);

/*
 * Runs the harness against the control socket getenv names as CONFORMANCE_SOCKET, until it is told to
 * finish, its Session ends, or it is asked to stop; its exit status.
 */
int harness_serve(yoke_getenv getenv, void *context);

#endif
