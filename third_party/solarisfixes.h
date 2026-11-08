/*
 * solarisfixes.h - local stand-in for the Redis Solaris compatibility header.
 *
 * This is NOT an upstream file. `zmalloc.c` includes "solarisfixes.h"
 * unconditionally, but upstream only uses it to work around Solaris
 * declarations when __sun is defined. This package targets the platforms
 * ct supports, none of which is Solaris, so the stand-in is empty.
 *
 * See third_party/README.md for the provenance of every file here.
 */
#ifndef CT_REDIS_LOCAL_SOLARISFIXES_H
#define CT_REDIS_LOCAL_SOLARISFIXES_H

/* deliberately empty */

#endif
