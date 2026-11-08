# third_party — Redis 7.2.4 dict snapshot

This directory is a byte-for-byte extract of the dict implementation from
[redis/redis](https://github.com/redis/redis) tag **7.2.4**, plus three
package-local adapter headers. It is not a Redis distribution and is not
endorsed by Redis. See `COPYING` and the copyright header of every source file.

Source archive: `https://download.redis.io/releases/redis-7.2.4.tar.gz`
sha256 `8d104c26a154b29fd67d6568b4f375212212ad41e0c2caa3d66480e78dbd3b59`.

Only 7.2.x may live here: 7.2 and earlier are BSD-3-Clause, 7.4+ is
RSALv2/SSPL and 8.0+ is AGPLv3, none of which may be vendored into this
package.

## Upstream files (unmodified)

| File | Why it is here |
| --- | --- |
| `dict.c` | the table: insert/find/delete, incremental rehash, safe iterator |
| `dict.h` | public types and `dictType`; also includes `mt19937-64.h` |
| `siphash.c` | `dictGenHashFunction` on 64-bit builds |
| `zmalloc.c` / `zmalloc.h` | the allocator `dict.c` calls; OOM aborts here |
| `mt19937-64.c` / `mt19937-64.h` | the hash seed and `randomULong` used by `dict.h` |
| `fmacros.h` | first include of `dict.c`, sets feature macros |
| `atomicvar.h` | pulled in by `zmalloc.c` |
| `COPYING` | BSD-3-Clause, kept verbatim |

Note: 7.2.4 has no `siphash.h`; `dict.c` declares the two siphash entry points
itself. `dict.h` only needs `siphash.c` linked.

## Package-local adapters (NOT upstream)

| File | Reason | What it does |
| --- | --- | --- |
| `redisassert.h` | upstream includes `config.h` / `server.h` helpers | self-contained `assert` / `panic` that print and `abort()` |
| `config.h` | `zmalloc.c` includes it unconditionally; Redis generates it at build time | deliberately empty: no Redis build feature flags leak into the package; `zmalloc.h` still derives `HAVE_MALLOC_SIZE` itself on macOS |
| `solarisfixes.h` | `zmalloc.c` includes it unconditionally | deliberately empty; only `__sun` needs the real one |

The adapters change no upstream line. `docs/implementation.md` records the
include closure and the compile probe.

## Integrity

`tools/third_party.sha256` covers every file here, including the adapters.
`tools/check.sh` runs `shasum -a 256 -c` against it. A changed upstream file is
a gate failure, not a local fix: re-extract from the archive and update the
manifest only when the tag itself changes.
