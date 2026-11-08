/*
 * config.h - local stand-in for the Redis build configuration.
 *
 * This is NOT an upstream file. `zmalloc.c` includes "config.h"
 * unconditionally (Redis generates it from config.h.in during its own build),
 * but this snapshot needs no Redis build feature flags: `zmalloc.h` derives
 * `HAVE_MALLOC_SIZE` itself (malloc_size on macOS, malloc_usable_size on
 * glibc, and the portable size-prefix path elsewhere). Keeping the file empty
 * means the package compiles with no generated configuration at all.
 *
 * See third_party/README.md for the provenance of every file here.
 */
#ifndef CT_REDIS_LOCAL_CONFIG_H
#define CT_REDIS_LOCAL_CONFIG_H

/* deliberately empty */

#endif
