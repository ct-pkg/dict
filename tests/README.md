# tests

S0 placeholder. No runner yet: the package has no implementation and no
`third_party/` snapshot, so there is nothing to build.

Planned suites (S2, see [../docs/design.md](../docs/design.md) §10):

| Suite | Covers |
| --- | --- |
| `strptr.ct` | `DictStrPtr`: put / get / del / len / clear / reserve |
| `borrow.ct` | the same key pointer registered twice; mutating key bytes makes it unreachable (negative case) |
| `intptr.ct` | `DictIntPtr`: 4-byte key slots are allocated and freed exactly once per entry |
| `iter.ct` | every key visited once; `removeCurrent()` in the middle, iteration continues |
| `rehash.ct` | 100k mixed put/get while rehashing; old keys always readable |
| `abort.ct` | documented OOM behaviour is abort, not an error code (kept out of the default run) |

`run.sh` will build every suite with `-Wall -Wextra` and fail on any warning.
The key-slot accounting gate must be proven by mutation: removing the
`keyDestructor` for `DictIntPtr` has to turn the gate red, otherwise the gate is
not evidence.
