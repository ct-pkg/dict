/*
 * redisassert.h - local adapter for the vendored Redis 7.2.4 snapshot.
 *
 * This is NOT an upstream file. Upstream src/redisassert.h pulls in
 * "config.h" for `likely()` and `redis_unreachable()`, both of which are
 * server-level helpers; vendoring it would drag the Redis server into the
 * package. dict.c includes <redisassert.h> but the API surface this package
 * exposes never reaches an assert(), so the adapter only has to exist and to
 * fail loudly if it is ever hit.
 *
 * Behaviour matches upstream: print file/line/expression, then abort. It does
 * not raise SIGSEGV like upstream's _serverAssert, because a package must not
 * turn a logic error into a signal that a caller might mistake for a memory
 * fault; abort() keeps the OOM contract and this contract on the same exit
 * path.
 *
 * See third_party/README.md for the provenance of every file here.
 */
#ifndef __REDIS_ASSERT_H__
#define __REDIS_ASSERT_H__

#include <stdio.h>
#include <stdlib.h>

static inline void _serverAssert(const char *estr, const char *file, int line)
{
    fprintf(stderr, "dict: assertion failed at %s:%d: %s\n", file, line, estr);
    fflush(stderr);
    abort();
}

static inline void _serverPanic(const char *file, int line, const char *msg)
{
    fprintf(stderr, "dict: panic at %s:%d: %s\n", file, line, msg ? msg : "");
    fflush(stderr);
    abort();
}

#define assert(_e) ((_e) ? (void)0 : (_serverAssert(#_e, __FILE__, __LINE__), abort()))
#define panic(msg) (_serverPanic(__FILE__, __LINE__, (msg)), abort())
#define redis_unreachable() abort()

#endif
