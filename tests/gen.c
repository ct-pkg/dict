/*
 * gen.c - the generation guard, built with AddressSanitizer by tests/run.sh.
 *
 * Clearing or releasing a table while an iterator is alive used to leave the
 * iterator walking freed entries (dictNext reads the old dictEntry). The shim
 * now bumps a reference-counted generation on clear()/release(); next(), key(),
 * value() and removeCurrent() see the mismatch and return empty. Under ASan any
 * remaining read of the freed table or entry aborts the process.
 *
 * License: BSD-3-Clause (see LICENSE).
 */

#include <stdio.h>

#include "Dict.shim.h"

static int passed = 0;
static int failed = 0;

static void check(int cond, const char *name)
{
    if (cond) {
        passed = passed + 1;
        return;
    }
    failed = failed + 1;
    printf("FAIL %s\n", name);
}

int main(void)
{
    ct_dict_intptr *h;
    ct_dict_intptr_iter *it;
    int value = 7;
    int i;

    /* ---------------- clear() during iteration ---------------- */

    h = ct_dict_intptr_new();
    for (i = 0; i < 16; i = i + 1) {
        ct_dict_intptr_put(h, i, 0);
    }
    it = ct_dict_intptr_new_iter(h);
    check(ct_dict_intptr_iter_next(it) == 1, "next yields an entry");
    ct_dict_intptr_clear(h);
    check(ct_dict_intptr_iter_next(it) == 0, "clear() invalidates the iterator");
    check(ct_dict_intptr_iter_value(it) == 0, "value() is empty after clear()");
    ct_dict_intptr_iter_release(it);
    check(ct_dict_intptr_len(h) == 0, "the table survives clear()");
    ct_dict_intptr_release(h);

    /* ---------------- release() during iteration ---------------- */

    h = ct_dict_intptr_new();
    ct_dict_intptr_put(h, 1, &value);
    it = ct_dict_intptr_new_iter(h);
    check(ct_dict_intptr_iter_next(it) == 1, "next yields the entry before release");
    ct_dict_intptr_release(h);
    check(ct_dict_intptr_iter_next(it) == 0, "release() invalidates the iterator");
    check(ct_dict_intptr_iter_value(it) == 0, "value() is empty after release()");
    check(ct_dict_intptr_iter_remove_current(it) == CT_DICT_NOT_FOUND,
          "removeCurrent() after release does nothing");
    ct_dict_intptr_iter_release(it);

    printf("GEN passed=%d failed=%d\n", passed, failed);
    return failed != 0;
}
