/*
 * Dict.shim.h - the C side of the dict package for the vendored Redis 7.2.4
 * dict.
 *
 * Two monomorphic shapes share one implementation, differing only in the
 * dictType (hash, compare, key destructor) and in the key type:
 *
 *   ct_dict_strptr  copied char * key      -> borrowed void * value
 *   ct_dict_intptr  int key (owned slot)   -> borrowed void * value
 *
 * The CT side never names `dict`, `dictEntry` or `dictIterator`; every public
 * method lands on one of the four wrapper structs below, and the mapping in
 * Dict.m.ct forwards to the ct_dict_* functions here.
 *
 * Ownership, restated because it is the point of this package:
 *
 *   - KEYS ARE COPIED AND OWNED BY THE TABLE. str keys are duplicated with
 *     zmalloc(strlen + 1) and freed by the dictType keyDestructor on delete,
 *     clear and release. int keys live in a 4-byte zmalloc slot, also freed by
 *     the keyDestructor. This removes the dangling-key risk of keeping a
 *     caller-owned pointer across an overwrite: the first copy stays valid
 *     even if the caller frees or mutates the buffer it passed in.
 *   - VALUES ARE POINTERS ONLY. The table never copies or frees them.
 *   - dictReplace() keeps the ORIGINAL key pointer when the key already
 *     exists; only the value is replaced. With copied keys that pointer is the
 *     first owned copy, which is safe. See README §5.2 and Q1.
 *
 * Failures: OOM follows Redis zmalloc, i.e. abort. reserve() separates the
 * two causes: the shim first mirrors _dictExpand()'s logical refusal
 * predicates (rehashing, buckets below ht_used[0], same power-of-two, size
 * overflow) and returns CT_DICT_INVALID without touching the allocator; only
 * when none holds does it call dictTryExpand(), where a DICT_ERR is a real
 * allocation failure and goes through the zmalloc OOM handler, i.e. abort.
 * The slot-count delta is still checked afterwards because 7.2.4's
 * dictTryExpand() reports DICT_OK even when _dictExpand() refused (Q3).
 *
 * Not part of the upstream snapshot: this file is the package's own adapter
 * layer, like Json.shim.h in the json package.
 */
#ifndef CT_DICT_SHIM_H
#define CT_DICT_SHIM_H

#include "dict.h"
#include "zmalloc.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* dict.h defines DICT_OK / DICT_ERR. The generated C also carries the
 * DictStatus enum from DictTypes.h.ct, whose members are named DICT_OK,
 * DICT_NOT_FOUND and DICT_INVALID. Without this undef the enum would be
 * textually rewritten by the macro and would not compile. The CT-visible
 * codes are mirrored under CT_ names so the shim never uses the macro form. */
#undef DICT_OK
#undef DICT_ERR

#define CT_DICT_OK        0
#define CT_DICT_NOT_FOUND 1
#define CT_DICT_INVALID   2

/* dictDelete()/dictAdd() report success as upstream DICT_OK, which is 0. The
 * macro itself cannot be used after the undef above, so name the value. */
#define CT_DICT_UPSTREAM_OK 0

typedef struct ct_dict_strptr ct_dict_strptr;
typedef struct ct_dict_intptr ct_dict_intptr;
typedef struct ct_dict_strptr_iter ct_dict_strptr_iter;
typedef struct ct_dict_intptr_iter ct_dict_intptr_iter;
typedef struct ct_dict_gen ct_dict_gen;

/* A handle and every live iterator share this counter. clear() and release()
 * bump it (release also marks it dead); an iterator checks it before touching
 * its dictIterator, so a table that was cleared or freed while iterating turns
 * next()/key()/value() into 0 instead of a heap-use-after-free. The guard is
 * reference-counted, which is what lets the iterator read the flag even after
 * the handle itself is gone. */
struct ct_dict_gen {
    unsigned long value;
    int refs;
    int dead;
};

struct ct_dict_strptr {
    dict *d;
    ct_dict_gen *gen;
    int status;
};

struct ct_dict_intptr {
    dict *d;
    ct_dict_gen *gen;
    int status;
};

/* A safe iterator: dictNext() prefetches the next entry, so removeCurrent()
 * may delete what was just returned and iteration still continues. `cur` is
 * the entry handed out by the last next(); key() / value() borrow from it and
 * turn into 0 after removeCurrent(), as the contract requires. */
struct ct_dict_strptr_iter {
    dictIterator *it;
    dictEntry *cur;
    ct_dict_gen *gen;
    unsigned long seen;
};

struct ct_dict_intptr_iter {
    dictIterator *it;
    dictEntry *cur;
    ct_dict_gen *gen;
    unsigned long seen;
};

static inline ct_dict_gen *ct_dict_gen_new(void)
{
    ct_dict_gen *g = zmalloc(sizeof(*g));

    g->value = 0;
    g->refs = 1;
    g->dead = 0;
    return g;
}

static inline void ct_dict_gen_hold(ct_dict_gen *g)
{
    g->refs = g->refs + 1;
}

static inline void ct_dict_gen_drop(ct_dict_gen *g)
{
    g->refs = g->refs - 1;
    if (g->refs == 0) {
        zfree(g);
    }
}

/* True while the iterator was created before the last clear()/release(). */
static inline int ct_dict_gen_valid(ct_dict_gen *g, unsigned long seen)
{
    return g->value == seen;
}

/* ------------------------------------------------------------------ */
/* dictType for the two shapes                                         */
/* ------------------------------------------------------------------ */

static inline uint64_t ct_dict_str_hash(const void *key)
{
    return dictGenHashFunction(key, strlen((const char *)key));
}

static inline int ct_dict_str_compare(dict *d, const void *key1, const void *key2)
{
    (void)d;
    return strcmp((const char *)key1, (const char *)key2) == 0;
}

/* String keys are copied into an owned buffer: the caller may free or mutate
 * its own buffer after put() without affecting the entry. The destructor runs
 * on del(), clear() and release(), so every live copy is paired exactly once. */
static inline char *ct_dict_str_key_dup(const char *key)
{
    size_t len = strlen(key) + 1;
    char *copy = zmalloc(len);

    memcpy(copy, key, len);
    return copy;
}

static inline void ct_dict_str_key_free(dict *d, void *key)
{
    (void)d;
    zfree(key);
}

static dictType ct_dict_str_type = {
    .hashFunction = ct_dict_str_hash,
    .keyCompare = ct_dict_str_compare,
    .keyDestructor = ct_dict_str_key_free,
};

static inline uint64_t ct_dict_int_hash(const void *key)
{
    return dictGenHashFunction(key, sizeof(int));
}

static inline int ct_dict_int_compare(dict *d, const void *key1, const void *key2)
{
    (void)d;
    return *(const int *)key1 == *(const int *)key2;
}

/* The 4-byte slot is the shim's only owned allocation inside an entry. */
static inline void ct_dict_int_key_free(dict *d, void *key)
{
    (void)d;
    zfree(key);
}

static dictType ct_dict_int_type = {
    .hashFunction = ct_dict_int_hash,
    .keyCompare = ct_dict_int_compare,
    .keyDestructor = ct_dict_int_key_free,
};

/*
 * The process-wide hash seed. Redis seeds it at startup from mt19937-64; this
 * package has no such startup hook, so the first table construction seeds it
 * from the clock and one stack address. There is no setter: dictType carries
 * no per-table seed in 7.2.4, and reproducing Redis' cross-table seed is not
 * useful to a consumer. Tests never depend on bucket order.
 *
 * The seeding function lives in seed.c, not here: a per-header static
 * latch would give every translation unit its own "already seeded" flag, and
 * the second unit to construct a table would re-randomise the process-global,
 * orphaning every key the first unit inserted.
 */
void ct_dict_seed_once(void);

/* ------------------------------------------------------------------ */
/* reserve: logical refusal vs real allocation failure                 */
/* ------------------------------------------------------------------ */

static inline unsigned long ct_dict_next_pow2(unsigned long n)
{
    unsigned long target = 1;

    while (target < n) {
        target <<= 1;
    }
    return target;
}

/* Mirror the refusal predicates of _dictExpand() in 7.2.4. A logical refusal
 * must be reported as DICT_INVALID without asking the allocator; otherwise a
 * real allocation failure would be indistinguishable from "nothing to do". */
static inline int ct_dict_expand_refused(dict *d, unsigned long buckets)
{
    unsigned long target;

    if (dictIsRehashing(d)) {
        return 1;
    }
    if (d->ht_used[0] > buckets) {
        return 1;
    }
    target = ct_dict_next_pow2(buckets);
    if (target > (unsigned long)-1 / sizeof(dictEntry *)) {
        return 1;
    }
    if (d->ht_table[0] != 0 && target == dictSlots(d)) {
        return 1;
    }
    return 0;
}

static inline void ct_dict_abort_oom(void)
{
    /* Go through zmalloc so the process-wide handler decides the message and
     * the abort. SIZE_MAX is rejected before malloc, which is exactly the
     * handler path used by every other allocation in this package. */
    (void)zmalloc((size_t)-1);
    abort();
}

static inline int ct_dict_reserve_status(dict *d, int buckets)
{
    unsigned long before;

    if (!d || buckets <= 0) {
        return CT_DICT_INVALID;
    }
    if (ct_dict_expand_refused(d, (unsigned long)buckets)) {
        return CT_DICT_INVALID;
    }
    before = dictSlots(d);
    if (dictTryExpand(d, (unsigned long)buckets) != CT_DICT_UPSTREAM_OK) {
        ct_dict_abort_oom();
    }
    /* belt and braces: 7.2.4's dictTryExpand() can report OK without growing
     * (Q3); anything left here is a logical refusal, not OOM. */
    return dictSlots(d) > before ? CT_DICT_OK : CT_DICT_INVALID;
}

/* ------------------------------------------------------------------ */
/* DictStrPtr                                                          */
/* ------------------------------------------------------------------ */

static inline ct_dict_strptr *ct_dict_strptr_new(void)
{
    ct_dict_strptr *h;

    ct_dict_seed_once();
    h = zmalloc(sizeof(*h));
    h->d = dictCreate(&ct_dict_str_type);
    h->gen = ct_dict_gen_new();
    h->status = CT_DICT_OK;
    return h;
}

static inline ct_dict_strptr *ct_dict_strptr_put(ct_dict_strptr *h, const char *key, void *value)
{
    dictEntry *e;
    char *copy;

    if (!h) {
        return 0;
    }
    if (!key) {
        h->status = CT_DICT_INVALID;
        return h;
    }
    /* On a hit keep the first owned copy (upstream dictReplace behaviour) and
     * only replace the value; no new key allocation, no slot churn. */
    e = dictFind(h->d, key);
    if (e) {
        dictSetVal(h->d, e, value);
        h->status = CT_DICT_OK;
        return h;
    }
    copy = ct_dict_str_key_dup(key);
    if (dictAdd(h->d, copy, value) != CT_DICT_UPSTREAM_OK) {
        zfree(copy);
        h->status = CT_DICT_INVALID;
        return h;
    }
    h->status = CT_DICT_OK;
    return h;
}

static inline int ct_dict_strptr_get(ct_dict_strptr *h, const char *key, void **out)
{
    dictEntry *e;

    if (!h || !key) {
        if (h) {
            h->status = CT_DICT_INVALID;
        }
        return CT_DICT_INVALID;
    }
    e = dictFind(h->d, key);
    if (!e) {
        h->status = CT_DICT_NOT_FOUND;
        return CT_DICT_NOT_FOUND;
    }
    if (out) {
        *out = dictGetVal(e);
    }
    h->status = CT_DICT_OK;
    return CT_DICT_OK;
}

static inline int ct_dict_strptr_del(ct_dict_strptr *h, const char *key)
{
    if (!h || !key) {
        if (h) {
            h->status = CT_DICT_INVALID;
        }
        return CT_DICT_INVALID;
    }
    h->status = dictDelete(h->d, key) == CT_DICT_UPSTREAM_OK ? CT_DICT_OK : CT_DICT_NOT_FOUND;
    return h->status;
}

static inline int ct_dict_strptr_len(ct_dict_strptr *h)
{
    return h ? (int)dictSize(h->d) : 0;
}

static inline ct_dict_strptr *ct_dict_strptr_clear(ct_dict_strptr *h)
{
    if (!h) {
        return 0;
    }
    dictEmpty(h->d, 0);
    h->gen->value = h->gen->value + 1;
    h->status = CT_DICT_OK;
    return h;
}

static inline ct_dict_strptr *ct_dict_strptr_reserve(ct_dict_strptr *h, int buckets)
{
    if (!h) {
        return 0;
    }
    h->status = ct_dict_reserve_status(h->d, buckets);
    return h;
}

static inline int ct_dict_strptr_status(ct_dict_strptr *h)
{
    return h ? h->status : CT_DICT_INVALID;
}

static inline ct_dict_strptr_iter *ct_dict_strptr_new_iter(ct_dict_strptr *h)
{
    ct_dict_strptr_iter *it;

    if (!h) {
        return 0;
    }
    it = zmalloc(sizeof(*it));
    it->it = dictGetSafeIterator(h->d);
    it->cur = 0;
    it->gen = h->gen;
    it->seen = h->gen->value;
    ct_dict_gen_hold(it->gen);
    return it;
}

static inline void ct_dict_strptr_release(ct_dict_strptr *h)
{
    if (!h) {
        return;
    }
    if (h->d) {
        dictRelease(h->d);
    }
    h->gen->value = h->gen->value + 1;
    h->gen->dead = 1;
    ct_dict_gen_drop(h->gen);
    zfree(h);
}

static inline int ct_dict_strptr_iter_next(ct_dict_strptr_iter *it)
{
    if (!it || !it->it) {
        return 0;
    }
    if (!ct_dict_gen_valid(it->gen, it->seen)) {
        it->cur = 0;
        return 0;
    }
    it->cur = dictNext(it->it);
    return it->cur ? 1 : 0;
}

static inline char *ct_dict_strptr_iter_key(ct_dict_strptr_iter *it)
{
    if (!it || !it->cur || !ct_dict_gen_valid(it->gen, it->seen)) {
        return 0;
    }
    return (char *)dictGetKey(it->cur);
}

static inline void *ct_dict_strptr_iter_value(ct_dict_strptr_iter *it)
{
    if (!it || !it->cur || !ct_dict_gen_valid(it->gen, it->seen)) {
        return 0;
    }
    return dictGetVal(it->cur);
}

static inline int ct_dict_strptr_iter_remove_current(ct_dict_strptr_iter *it)
{
    void *key;
    int rc;

    if (!it || !it->cur || !ct_dict_gen_valid(it->gen, it->seen)) {
        return CT_DICT_NOT_FOUND;
    }
    key = dictGetKey(it->cur);
    it->cur = 0;
    rc = dictDelete(it->it->d, key);
    return rc == CT_DICT_UPSTREAM_OK ? CT_DICT_OK : CT_DICT_NOT_FOUND;
}

static inline void ct_dict_strptr_iter_release(ct_dict_strptr_iter *it)
{
    if (!it) {
        return;
    }
    if (it->it) {
        if (it->gen->dead) {
            zfree(it->it);
        } else {
            dictReleaseIterator(it->it);
        }
    }
    ct_dict_gen_drop(it->gen);
    zfree(it);
}

/* ------------------------------------------------------------------ */
/* DictIntPtr                                                          */
/* ------------------------------------------------------------------ */

static inline ct_dict_intptr *ct_dict_intptr_new(void)
{
    ct_dict_intptr *h;

    ct_dict_seed_once();
    h = zmalloc(sizeof(*h));
    h->d = dictCreate(&ct_dict_int_type);
    h->gen = ct_dict_gen_new();
    h->status = CT_DICT_OK;
    return h;
}

/* The key slot is allocated only when the key is new: on a hit the existing
 * entry is updated in place, so the slot count stays exactly one per live
 * entry. Using dictReplace() here would allocate a fresh slot for a hit and
 * leave it unused (dictReplace keeps the original key), i.e. one leaked slot
 * per overwrite. */
static inline ct_dict_intptr *ct_dict_intptr_put(ct_dict_intptr *h, int key, void *value)
{
    dictEntry *e;
    int *slot;

    if (!h) {
        return 0;
    }
    e = dictFind(h->d, &key);
    if (e) {
        dictSetVal(h->d, e, value);
        h->status = CT_DICT_OK;
        return h;
    }
    slot = zmalloc(sizeof(*slot));
    *slot = key;
    if (dictAdd(h->d, slot, value) != CT_DICT_UPSTREAM_OK) {
        zfree(slot);
        h->status = CT_DICT_INVALID;
        return h;
    }
    h->status = CT_DICT_OK;
    return h;
}

static inline int ct_dict_intptr_get(ct_dict_intptr *h, int key, void **out)
{
    dictEntry *e;

    if (!h) {
        return CT_DICT_INVALID;
    }
    e = dictFind(h->d, &key);
    if (!e) {
        h->status = CT_DICT_NOT_FOUND;
        return CT_DICT_NOT_FOUND;
    }
    if (out) {
        *out = dictGetVal(e);
    }
    h->status = CT_DICT_OK;
    return CT_DICT_OK;
}

static inline int ct_dict_intptr_del(ct_dict_intptr *h, int key)
{
    if (!h) {
        return CT_DICT_INVALID;
    }
    h->status = dictDelete(h->d, &key) == CT_DICT_UPSTREAM_OK ? CT_DICT_OK : CT_DICT_NOT_FOUND;
    return h->status;
}

static inline int ct_dict_intptr_len(ct_dict_intptr *h)
{
    return h ? (int)dictSize(h->d) : 0;
}

static inline ct_dict_intptr *ct_dict_intptr_clear(ct_dict_intptr *h)
{
    if (!h) {
        return 0;
    }
    dictEmpty(h->d, 0);
    h->gen->value = h->gen->value + 1;
    h->status = CT_DICT_OK;
    return h;
}

static inline ct_dict_intptr *ct_dict_intptr_reserve(ct_dict_intptr *h, int buckets)
{
    if (!h) {
        return 0;
    }
    h->status = ct_dict_reserve_status(h->d, buckets);
    return h;
}

static inline int ct_dict_intptr_status(ct_dict_intptr *h)
{
    return h ? h->status : CT_DICT_INVALID;
}

static inline ct_dict_intptr_iter *ct_dict_intptr_new_iter(ct_dict_intptr *h)
{
    ct_dict_intptr_iter *it;

    if (!h) {
        return 0;
    }
    it = zmalloc(sizeof(*it));
    it->it = dictGetSafeIterator(h->d);
    it->cur = 0;
    it->gen = h->gen;
    it->seen = h->gen->value;
    ct_dict_gen_hold(it->gen);
    return it;
}

static inline void ct_dict_intptr_release(ct_dict_intptr *h)
{
    if (!h) {
        return;
    }
    if (h->d) {
        dictRelease(h->d);
    }
    h->gen->value = h->gen->value + 1;
    h->gen->dead = 1;
    ct_dict_gen_drop(h->gen);
    zfree(h);
}

static inline int ct_dict_intptr_iter_next(ct_dict_intptr_iter *it)
{
    if (!it || !it->it) {
        return 0;
    }
    if (!ct_dict_gen_valid(it->gen, it->seen)) {
        it->cur = 0;
        return 0;
    }
    it->cur = dictNext(it->it);
    return it->cur ? 1 : 0;
}

static inline int ct_dict_intptr_iter_key(ct_dict_intptr_iter *it)
{
    if (!it || !it->cur || !ct_dict_gen_valid(it->gen, it->seen)) {
        return 0;
    }
    return *(int *)dictGetKey(it->cur);
}

static inline void *ct_dict_intptr_iter_value(ct_dict_intptr_iter *it)
{
    if (!it || !it->cur || !ct_dict_gen_valid(it->gen, it->seen)) {
        return 0;
    }
    return dictGetVal(it->cur);
}

static inline int ct_dict_intptr_iter_remove_current(ct_dict_intptr_iter *it)
{
    void *key;
    int rc;

    if (!it || !it->cur || !ct_dict_gen_valid(it->gen, it->seen)) {
        return CT_DICT_NOT_FOUND;
    }
    key = dictGetKey(it->cur);
    it->cur = 0;
    rc = dictDelete(it->it->d, key);
    return rc == CT_DICT_UPSTREAM_OK ? CT_DICT_OK : CT_DICT_NOT_FOUND;
}

static inline void ct_dict_intptr_iter_release(ct_dict_intptr_iter *it)
{
    if (!it) {
        return;
    }
    if (it->it) {
        if (it->gen->dead) {
            zfree(it->it);
        } else {
            dictReleaseIterator(it->it);
        }
    }
    ct_dict_gen_drop(it->gen);
    zfree(it);
}

#endif
