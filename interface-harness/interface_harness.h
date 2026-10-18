/*
 * The C interface library's harness: the thinnest translation between the suite's directives and the
 * library. Launched by the suite against an instance, it attaches to the channel a directive names at the
 * address the library computes, turns each further directive into one call of the library, reports what
 * the library answered in the suite's vocabulary — a refusal as its code and what it names, never the
 * library's words — and what the standing subscription or a subscription delivers as observations, in
 * order. It judges nothing, confirms only when told to, and speaks no wire of its own.
 */
#ifndef YOKE_INTERFACE_HARNESS_H
#define YOKE_INTERFACE_HARNESS_H

#include "yoke/interface.h"

/*
 * Runs the harness against the control socket getenv names as CONFORMANCE_SOCKET, with the instance's root
 * in CONFORMANCE_INSTANCE, until it is told to finish; its exit status.
 */
int interface_harness_serve(yoke_getenv getenv, void *context);

#endif
