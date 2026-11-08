/*
 * multi_tu_a.c - first translation unit of the seed gate.
 *
 * Inserts eight keys, then asks the second unit to construct another table.
 * With a per-header seed latch that second construction reseeds the
 * process-global and every key here becomes unreachable; the fixed build keeps
 * one seed for the whole process.
 *
 * License: BSD-3-Clause (see LICENSE).
 */

#include <stdio.h>

#include "Dict.shim.h"

void other_tu_build_table(void);

static int all_present(ct_dict_intptr *h)
{
    void *out;
    int i;

    for (i = 0; i < 8; i = i + 1) {
        if (ct_dict_intptr_get(h, i, &out) != CT_DICT_OK) {
            return 0;
        }
    }
    return 1;
}

int main(void)
{
    ct_dict_intptr *first;
    int values[8];
    int i;
    int passed = 0;
    int failed = 0;

    first = ct_dict_intptr_new();
    for (i = 0; i < 8; i = i + 1) {
        values[i] = i;
        ct_dict_intptr_put(first, i, &values[i]);
    }

    other_tu_build_table();

    if (all_present(first)) {
        passed = passed + 1;
    } else {
        failed = failed + 1;
        printf("FAIL the second translation unit reseeded and orphaned the keys\n");
    }

    ct_dict_intptr_release(first);
    printf("MULTITU passed=%d failed=%d\n", passed, failed);
    return failed != 0;
}
