/* The C interface harness, as the suite launches it. */
#include <stdlib.h>

#include "interface_harness.h"

static const char *environment(const char *name, void *context) {
    (void)context;
    return getenv(name);
}

int main(void) { return interface_harness_serve(environment, NULL); }
