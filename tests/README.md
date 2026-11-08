# tests

`run.sh` builds and runs every suite, in a temporary directory, with
`-Wall -Wextra`; any `warning:`, a missing all-pass summary, or a passed count
below the suite's measured floor fails the run. Floors: unit 68, iterator 343,
rehash 16, chain 17, slots 1536, oom 4, gen 8. Gutting a suite's `check()`
therefore turns the gate red instead of reporting "passed=2 failed=0". It also
builds and runs the C gates (including an AddressSanitizer build of `gen.c` and
the two-translation-unit seed gate), and diffs the demo.

| Suite | Covers |
| --- | --- |
| `unit.ct` | `DictStrPtr` / `DictIntPtr`: constructors, put / get / del / len / clear, overwrite, copied-key ownership (equal bytes at distinct addresses are one entry; the entry owns a copy, not the caller's buffer; overwriting keeps the first copy by pointer identity; mutating the caller's buffer does not affect the copy), null key/handle, reserve success and each logical refusal. 68 assertions. |
| `iterator.ct` | every key exactly once, `removeCurrent()` mid-iteration, removal of odd keys, put during iteration (original keys still visited), full sweep, reuse, string iterator contract, empty table. 343 assertions. |
| `rehash.ct` | 100k inserts with old-key re-reads every 1000 inserts, stride samples, iterator count, delete first/last; an iterator created while an incremental rehash is in progress must see every entry; a pre-reserved run checks load equivalence. Prints `REHASH grow` / `REHASH presized` / `REHASH equivalent` evidence lines. 16 assertions. |
| `chain.ct` | constructor recipient, four-step `put.put.reserve.clear` chain returning the receiver, captured receiver reuse, int variants. 17 assertions. |
| `slots.c` | allocation accounting with `zmalloc_used_memory()`: overwrite allocates no new key slot or key copy, delete/clear/release return to the expected baseline, the 1k-vs-10k differential is exact, and 120-byte string keys cost ~112 KB more than 4-byte keys (keys are copied). Also drives incremental rehash behaviour: it must be in progress mid-load (`dictIsRehashing` / `rehashidx >= 0`), `rehashidx` must never move backwards, it must finish (`ht_used[1] == 0`, `rehashidx == -1`), and `reserve()` during rehash must be a logical `DICT_INVALID`. 1536 assertions. |
| `oom.c` | deterministic OOM injection by interposing `malloc` / `calloc` / `realloc`: armed `put`, constructor and `reserve()` abort with the package marker; a logical `reserve()` refusal does not abort and reports `DICT_INVALID`. 4 assertions. |
| `gen.c` | generation guard under AddressSanitizer: `clear()` / `release()` while an iterator is alive must make `next()` / `key()` / `value()` return 0 exactly once, never read freed memory, and leave the handle usable (after `clear()`) or safely releasable (after `release()`). 8 assertions. |
| `multi_tu_a.c` / `multi_tu_b.c` | two translation units each include the shim; the first inserts keys, the second constructs another table (the old per-TU seed latch reseeded the process-global here), then the first table is read back. `run.sh` repeats it 20 times so same-second luck cannot hide the bug. |

`../tools/check.sh` runs this file plus the static gates. Each gate category's
mutation evidence is recorded in [../docs/implementation.md](../docs/implementation.md)
§7 (M1–M29).
