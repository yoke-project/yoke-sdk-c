/* The C plugin harness, as the suite and the Core launch it. */
#include <stdlib.h>

#include "harness.h"

static const char *environment(const char *name, void *context) {
    (void)context;
    return getenv(name);
}

int main(void) { return harness_serve(environment, NULL); }
