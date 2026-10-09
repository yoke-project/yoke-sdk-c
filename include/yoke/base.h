/*
 * What the libraries of this project share and nothing else: the addresses a party computes from its
 * environment, a refusal as C carries an error, and the contract version each library states.
 *
 * A function that can fail returns 0 when it succeeded and -1 when it did not, and fills the error it
 * is handed, if it is handed one. An error is a refusal when it carries a code from the one namespace;
 * a failure that is not one carries a message alone.
 */
#ifndef YOKE_BASE_H
#define YOKE_BASE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* What this project's libraries say they are: at admission, and in a harness's hello. */
#define YOKE_SDK_LINE "yoke-sdk-c 0.4.0"

/* What a refusal names: a kind, an identity, and a unit's life where one is named. */
typedef struct yoke_subject {
    char *kind;
    char *identity;
    uint64_t incarnation;
} yoke_subject;

/* An error. Every string is owned by the error, and yoke_error_clear releases them. */
typedef struct yoke_error {
    char *code;    /* the refusal's code, from the one namespace; NULL when it is not a refusal */
    char *message; /* for a person */
    char *stage;   /* the stage of admission a refusal was made at, where there is one */
    char *item;    /* the item withheld or undeclared, where it names one */
    yoke_subject subject;
} yoke_error;

/* Releases what an error holds, and leaves it empty. */
void yoke_error_clear(yoke_error *err);

/* Whether err is a refusal with the code given. */
bool yoke_is_refusal(const yoke_error *err, const char *code);

/* Reads one variable of the environment a party is handed; NULL when it is not set. */
typedef const char *(*yoke_getenv)(const char *name, void *context);

/* What a party is handed, and all it may assume. Every string is owned by it. */
typedef struct yoke_env {
    char *plugin; /* the plugin this unit is a copy of */
    char *unit;   /* this unit's identity */
    char *socket; /* the instance's plugin channel */
    char *bind;   /* the path this unit is expected to bind */
    char *token;  /* the bootstrap token */
} yoke_env;

/* Reads the reserved variables. A missing one is an error naming it; no path is assumed. */
int yoke_environment(yoke_env *env, yoke_getenv getenv, void *context, yoke_error *err);

/* Releases what an environment holds. */
void yoke_env_clear(yoke_env *env);

/* The version of the plugin contract the definitions carry, which the plugin library declares. */
int yoke_plugin_contract(void);

#ifdef __cplusplus
}
#endif

#endif
