/*
 * seed.c - the process-wide hash seed, seeded exactly once per process.
 *
 * dictSetHashFunctionSeed() writes a process-global. The latch must therefore
 * live in one translation unit, not in a header: a `static` latch in the shim
 * would give every .ct unit its own copy, so the second unit to build a table
 * would reseed the global and silently orphan keys inserted by the first.
 * Every unit calls this one non-static symbol instead.
 *
 * The file name is short on purpose: ct caps a ct.pkg metadata value at 160
 * bytes, and this path appears once in `ldflags` next to the four upstream
 * sources. A longer name overflows that cap ("metadata that does not fit").
 *
 * License: BSD-3-Clause (see LICENSE).
 */

#include "dict.h"

#include <stdint.h>
#include <time.h>

void ct_dict_seed_once(void)
{
    static int seeded = 0;
    uint64_t seed[2];
    unsigned long long mix;

    if (seeded) {
        return;
    }
    seeded = 1;

    mix = (unsigned long long)time(0);
    mix ^= (unsigned long long)(uintptr_t)&seed << 17;
    init_genrand64(mix);
    seed[0] = (uint64_t)genrand64_int64();
    seed[1] = (uint64_t)genrand64_int64();
    dictSetHashFunctionSeed((uint8_t *)seed);
}
