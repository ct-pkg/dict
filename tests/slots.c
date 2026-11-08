/*
 * slots.c - the accounting gate, in C because it needs zmalloc_used_memory().
 *
 * What it proves (each equality is exact; zmalloc's counter is deterministic
 * for an identical sequence of allocation sizes):
 *
 *   1. overwriting an int key does not allocate a new 4-byte key slot
 *      (upstream dictReplace() keeps the original key; using it here would
 *      allocate a slot per overwrite and leak it);
 *   2. deleting and re-inserting the same int keys returns to the same byte
 *      count, so each entry owns exactly one slot and frees it exactly once;
 *   3. clear() returns the table to its pre-insert byte count: no entry and no
 *      slot survives;
 *   4. release() returns to the pre-creation byte count: the table is freed too;
 *   5. a 1k-vs-10k differential: with the same pre-reserved bucket array, ten
 *      times the entries must cost exactly ten times the bytes. A fixed
 *      absolute threshold cannot see a small per-insert leak; this ratio can.
 *   6. string keys are copied and owned: 1000 keys of ~120 bytes cost about
 *      116 KB more than 1000 keys of ~4 bytes, and clear()/release() free the
 *      copies back to the baseline.
 *
 * Exit code 0 means every equality held.
 *
 * License: BSD-3-Clause (see LICENSE).
 */

#include <stdio.h>
#include <string.h>

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

static char short_keys[1000][16];
static char long_keys[1000][128];

static void make_keys(void)
{
    int i;
    int j;
    int n;

    for (i = 0; i < 1000; i = i + 1) {
        snprintf(short_keys[i], sizeof(short_keys[i]), "s%d", i);
        n = snprintf(long_keys[i], sizeof(long_keys[i]), "l%d:", i);
        for (j = n; j < 120; j = j + 1) {
            long_keys[i][j] = 'x';
        }
        long_keys[i][120] = 0;
    }
}

int main(void)
{
    size_t base;
    size_t handle;
    size_t reserved;
    size_t u1;
    size_t u2;
    size_t u3;
    size_t u4;
    size_t u5;
    size_t u6;
    size_t u7;
    size_t u8;
    size_t s_handle;
    size_t s_reserved;
    size_t s1;
    size_t s2;
    size_t l_handle;
    size_t l_reserved;
    size_t l1;
    size_t h2;
    size_t r1;
    size_t r2;
    size_t d1;
    size_t d10;
    size_t res1;
    size_t res10;
    ct_dict_intptr *m;
    ct_dict_strptr *ds;
    ct_dict_strptr *dl;
    int i;
    int j;

    make_keys();

    /* ---------------- int keys: one slot per live entry ---------------- */

    base = zmalloc_used_memory();
    m = ct_dict_intptr_new();
    check(zmalloc_used_memory() > base, "the handle allocation is counted");
    handle = zmalloc_used_memory();

    check(ct_dict_intptr_reserve(m, 4096) == m, "reserve returns the receiver");
    check(ct_dict_intptr_status(m) == CT_DICT_OK, "reserve(4096) succeeds");
    reserved = zmalloc_used_memory();

    for (i = 0; i < 1000; i = i + 1) {
        check(ct_dict_intptr_put(m, i, 0) == m, "int put returns the receiver");
    }
    u1 = zmalloc_used_memory();
    check(u1 > reserved, "1000 int entries allocate beyond the table");
    check(ct_dict_intptr_len(m) == 1000, "len is 1000 after the int load");

    /*
     * 10 overwrites per key. If put() allocated a fresh slot per overwrite
     * (the dictReplace trap), used_memory would grow by 10000 allocations.
     */
    for (i = 0; i < 1000; i = i + 1) {
        for (j = 0; j < 10; j = j + 1) {
            ct_dict_intptr_put(m, i, (void *)(long)j);
        }
    }
    u2 = zmalloc_used_memory();
    check(u2 == u1, "overwriting an int key allocates nothing (no slot leak)");

    for (i = 0; i < 1000; i = i + 2) {
        check(ct_dict_intptr_del(m, i) == CT_DICT_OK, "int del of an even key succeeds");
    }
    u3 = zmalloc_used_memory();
    check(u3 < u1, "500 deletes free entry and slot bytes");
    check(ct_dict_intptr_len(m) == 500, "500 entries remain after the deletes");
    check(ct_dict_intptr_get(m, 0, 0) == CT_DICT_NOT_FOUND, "a deleted key is not found");
    check(ct_dict_intptr_get(m, 1, 0) == CT_DICT_OK, "an odd key survived");

    for (i = 0; i < 1000; i = i + 2) {
        ct_dict_intptr_put(m, i, 0);
    }
    u4 = zmalloc_used_memory();
    check(u4 == u1, "re-inserting the deleted keys returns to the same byte count");
    check(ct_dict_intptr_len(m) == 1000, "len is 1000 again");

    ct_dict_intptr_clear(m);
    u5 = zmalloc_used_memory();
    check(u5 == handle, "clear() frees every entry, every 4-byte slot and the bucket array");

    for (i = 0; i < 1000; i = i + 1) {
        ct_dict_intptr_put(m, i, 0);
    }
    u6 = zmalloc_used_memory();
    check(u6 > handle, "a second load allocates again from an empty table");
    check(ct_dict_intptr_len(m) == 1000, "the second load has 1000 entries");
    ct_dict_intptr_clear(m);
    u7 = zmalloc_used_memory();
    check(u7 == handle, "the second clear() is equally complete");

    ct_dict_intptr_release(m);
    u8 = zmalloc_used_memory();
    check(u8 == base, "release() frees the table and the handle");

    /* ---------------- 1k vs 10k differential ---------------- */
    /*
     * Same pre-reserved bucket array, 1000 then 10000 distinct entries. Each
     * entry costs one node plus one 4-byte slot, so the byte delta must scale
     * exactly 10x. This catches a leak proportional to the number of inserts
     * even when it is far below any absolute RSS threshold.
     */

    m = ct_dict_intptr_new();
    h2 = zmalloc_used_memory();
    ct_dict_intptr_reserve(m, 16384);
    r1 = zmalloc_used_memory();
    for (i = 0; i < 1000; i = i + 1) {
        ct_dict_intptr_put(m, i, 0);
    }
    d1 = zmalloc_used_memory();
    ct_dict_intptr_clear(m);
    res1 = zmalloc_used_memory() - h2;
    check(res1 == 0, "the differential clear() returns to the handle");

    ct_dict_intptr_reserve(m, 16384);
    r2 = zmalloc_used_memory();
    check(r2 == r1, "the same reserve() costs the same bytes twice");
    for (i = 0; i < 10000; i = i + 1) {
        ct_dict_intptr_put(m, i, 0);
    }
    d10 = zmalloc_used_memory();
    check(d10 - r2 == (d1 - r1) * 10,
          "1k vs 10k differential: per-entry bytes are constant (no per-insert leak)");
    ct_dict_intptr_clear(m);
    res10 = zmalloc_used_memory() - h2;
    check(res10 == res1,
          "1k vs 10k differential: clear() leaves the same residual at both sizes");
    printf("SLOTS 1k=%zu B 10k=%zu B reserved=%zu B residual1=%zu B residual10=%zu B\n",
           d1 - r1, d10 - r2, r1 - h2, res1, res10);
    ct_dict_intptr_release(m);
    check(zmalloc_used_memory() == base, "the differential table releases to the same baseline");

    /* ---------------- string keys: copied and owned ---------------- */

    ds = ct_dict_strptr_new();
    s_handle = zmalloc_used_memory();
    ct_dict_strptr_reserve(ds, 4096);
    s_reserved = zmalloc_used_memory();
    for (i = 0; i < 1000; i = i + 1) {
        ct_dict_strptr_put(ds, short_keys[i], 0);
    }
    s1 = zmalloc_used_memory();
    ct_dict_strptr_put(ds, short_keys[0], (void *)1);
    s2 = zmalloc_used_memory();
    check(s2 == s1, "overwriting a string key allocates nothing");
    ct_dict_strptr_clear(ds);
    check(zmalloc_used_memory() == s_handle, "string clear() frees the key copies too");
    ct_dict_strptr_release(ds);

    dl = ct_dict_strptr_new();
    l_handle = zmalloc_used_memory();
    check(l_handle == s_handle, "both string tables have the same handle footprint");
    ct_dict_strptr_reserve(dl, 4096);
    l_reserved = zmalloc_used_memory();
    check(l_reserved == s_reserved, "both string tables reserve the same bytes");
    for (i = 0; i < 1000; i = i + 1) {
        ct_dict_strptr_put(dl, long_keys[i], 0);
    }
    l1 = zmalloc_used_memory();
    check(l1 - l_reserved > s1 - s_reserved,
          "120-byte keys cost more than 4-byte keys: keys are copied");
    check(l1 - l_reserved - (s1 - s_reserved) >= 1000 * 90,
          "the extra bytes track the key length (at least 90 B per long key)");
    check(l1 - l_reserved - (s1 - s_reserved) <= 1000 * 130,
          "the extra bytes track the key length (at most 130 B per long key)");
    ct_dict_strptr_clear(dl);
    check(zmalloc_used_memory() == l_handle, "clearing long keys frees every copy");
    ct_dict_strptr_release(dl);
    check(zmalloc_used_memory() == base, "both string tables release to the same baseline");
    printf("SLOTS str short=%zu B long=%zu B delta=%zu B\n",
           s1 - s_reserved, l1 - l_reserved, (l1 - l_reserved) - (s1 - s_reserved));

    /* ---------------- incremental rehash actually runs ---------------- */
    /*
     * Load 1000 entries into the small table, then reserve(65536): the rehash
     * starts with 1000 entries still in table 0, so it needs ~1000 steps and
     * cannot finish during the call. The next inserts advance it (find + add =
     * two steps each), and enough inserts drain it. Changing dict.c's
     * `dictRehash(d, 1)` to 0 leaves the table stuck in rehashidx 0 forever and
     * reserve() permanently refused.
     */
    {
        ct_dict_intptr *g = ct_dict_intptr_new();
        int prev = -1;
        int monotonic = 1;
        int at_1k = 0;
        int refused = 0;
        int drained = 0;

        for (i = 0; i < 1000; i = i + 1) {
            ct_dict_intptr_put(g, i, 0);
        }
        ct_dict_intptr_reserve(g, 65536);
        check(dictIsRehashing(g->d), "reserve starts an incremental rehash");

        at_1k = dictIsRehashing(g->d) && g->d->rehashidx >= 0;
        ct_dict_intptr_reserve(g, 131072);
        refused = ct_dict_intptr_status(g) == CT_DICT_INVALID;

        for (i = 1000; i < 4000; i = i + 1) {
            ct_dict_intptr_put(g, i, 0);
            if (g->d->rehashidx != -1) {
                if (g->d->rehashidx < prev) {
                    monotonic = 0;
                }
                prev = g->d->rehashidx;
            }
        }
        while (dictIsRehashing(g->d) && drained < 1000000) {
            ct_dict_intptr_get(g, 0, 0);
            drained = drained + 1;
        }
        check(at_1k, "rehash is in progress at 1000 entries");
        check(monotonic, "rehashidx never moves backwards");
        check(!dictIsRehashing(g->d) && g->d->ht_used[1] == 0,
              "rehash completes once enough operations run");
        check(refused, "reserve during rehash is a logical refusal");
        ct_dict_intptr_release(g);
    }

    printf("SLOTS passed=%d failed=%d\n", passed, failed);
    return failed != 0;
}
