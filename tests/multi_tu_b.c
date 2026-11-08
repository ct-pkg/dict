/*
 * multi_tu_b.c - second translation unit of the seed gate.
 *
 * Only constructs (and releases) a table. This is the action that used to
 * reseed from this unit's own static latch.
 *
 * License: BSD-3-Clause (see LICENSE).
 */

#include "Dict.shim.h"

void other_tu_build_table(void)
{
    ct_dict_intptr *second;

    second = ct_dict_intptr_new();
    ct_dict_intptr_put(second, 100, 0);
    ct_dict_intptr_release(second);
}
