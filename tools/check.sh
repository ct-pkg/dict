#!/usr/bin/env bash
#
# tools/check.sh - the dict package gate.
#
#   bash tools/check.sh
#   CT=/path/to/ct bash tools/check.sh
#
# Static checks first, then two behavioural gates (an installed-layout build and
# tests/run.sh). Every check that claims to catch a defect has a recorded
# mutation test in docs/implementation.md; a green run here is the result, not
# the evidence.
#
#   1. the delivered file set exists and every delivered file is tracked by git
#      (`git ls-files --error-unmatch`), so a clean clone is the package;
#   2. ct.pkg declares the five expected keys, requires ct >= 0.20.3, points at
#      third_party/ (never vendor/), and links the four upstream sources plus
#      dict/seed.c;
#   3. third_party/ matches tools/third_party.sha256 exactly, with no extra file
#      other than its README;
#   4. the licence boundary holds: BSD-3-Clause only, no 7.4+/8.x text;
#   5. the public surface is OO: no free `pub fn`, exactly four handle aliases,
#      no dict/C type names in trait signatures, every trait method mapped once,
#      and put/clear/reserve return the receiver;
#   6. the shim has no mutable static state (every definition is `static inline`
#      except the immutable dictType tables), the hash seed is only *declared*
#      there and defined in seed.c, it uses the safe iterator, and it
#      undefines the upstream DICT_OK/DICT_ERR macros before the status enum;
#   7. dict.c calls no raw allocator: OOM therefore goes through zmalloc's
#      handler (proved behaviourally by tests/oom.c);
#   8. README has exactly ten numbered sections in the agreed order, the
#      glossary is the three-column table before the limits, all three status
#      codes are named, and no C type leaks outside the glossary;
#   9. every `ct` block in README is a complete program: it is extracted, built
#      with -Wall -Wextra, run, and its stdout is compared with the expected
#      output recorded here;
#  10. a real `ct get` + `ct install` copies third_party/ and the installed demo
#      builds, runs and matches demo/expected.out;
#  11. tests/run.sh passes.
#
# Exit status: 0 only when every check passes.
#
# License: BSD-3-Clause (see LICENSE).

set -u

pkg="$(cd "$(dirname "$0")/.." && pwd)"
CT="${CT:-/Users/samxx/.local/bin/ct}"

pass=0
fail=0
ok()  { pass=$((pass + 1)); printf '  ok    %s\n' "$1"; }
bad() { fail=$((fail + 1)); printf '  FAIL  %s: %s\n' "$1" "$2"; }

work="${TMPDIR:-/tmp}/ct-dict-check.$$"
cleanup() { rm -rf "$work"; }
trap cleanup EXIT HUP INT TERM
mkdir -p "$work" || exit 1

echo "dict gate: $pkg"
echo

echo "== delivered files =="
delivered="ct.pkg LICENSE README.md
dict/Dict.m.ct dict/DictTypes.h.ct dict/Dict.shim.h dict/seed.c
third_party/dict.c third_party/dict.h third_party/siphash.c
third_party/zmalloc.c third_party/zmalloc.h
third_party/mt19937-64.c third_party/mt19937-64.h
third_party/fmacros.h third_party/atomicvar.h
third_party/config.h third_party/solarisfixes.h
third_party/redisassert.h third_party/COPYING third_party/README.md
tools/third_party.sha256 tools/check.sh
tests/run.sh tests/unit.ct tests/iterator.ct tests/rehash.ct tests/chain.ct
tests/slots.c tests/oom.c tests/gen.c tests/multi_tu_a.c tests/multi_tu_b.c
demo/main.ct demo/expected.out
docs/design.md docs/implementation.md"
for f in $delivered; do
    if [ -f "$pkg/$f" ]; then
        ok "file $f"
    else
        bad "file $f" "missing"
    fi
done
if ! git -C "$pkg" rev-parse --git-dir >/dev/null 2>&1; then
    bad "git delivery" "not a git checkout; cannot prove a clean clone has the package"
else
    untracked=""
    for f in $delivered; do
        if ! git -C "$pkg" ls-files --error-unmatch "$f" >/dev/null 2>&1; then
            untracked="$untracked $f"
        fi
    done
    if [ -z "$untracked" ]; then
        ok "every delivered file is tracked by git"
    else
        bad "git delivery" "delivered files are not tracked:$untracked"
    fi
fi
echo

echo "== ct.pkg =="
for key in name version licence cflags ldflags; do
    if grep -qE "^${key}[[:space:]]*=" "$pkg/ct.pkg"; then
        ok "field $key"
    else
        bad "field $key" "missing"
    fi
done
if grep -qE '^name[[:space:]]*=[[:space:]]*dict$' "$pkg/ct.pkg"; then
    ok "name is dict"
else
    bad "name" "must be dict"
fi
if grep -qE '^licence[[:space:]]*=[[:space:]]*BSD-3-Clause$' "$pkg/ct.pkg"; then
    ok "licence is BSD-3-Clause"
else
    bad "licence" "must be BSD-3-Clause"
fi
if grep -qE '^requires[[:space:]]*=[[:space:]]*ct[[:space:]]*>=[[:space:]]*0\.20\.3$' "$pkg/ct.pkg"; then
    ok "requires ct >= 0.20.3"
else
    bad "requires" "must be ct >= 0.20.3"
fi
if grep -q 'vendor/dict/third_party' "$pkg/ct.pkg"; then
    ok "ct.pkg points at vendor/dict/third_party"
else
    bad "ct.pkg paths" "cflags/ldflags must use vendor/dict/third_party"
fi
if grep -qE '^dep[[:space:]]*=' "$pkg/ct.pkg"; then
    bad "dep" "this package has no package dependencies"
else
    ok "no dep field"
fi
for src in dict.c siphash.c zmalloc.c mt19937-64.c; do
    if grep -q "vendor/dict/third_party/$src" "$pkg/ct.pkg"; then
        ok "ldflags names $src"
    else
        bad "ldflags $src" "missing"
    fi
done
if grep -q 'vendor/dict/dict/seed.c' "$pkg/ct.pkg"; then
    ok "ldflags names dict/seed.c"
else
    bad "ldflags dict/seed.c" "the seed unit must be linked, not inlined per TU"
fi
ldf=$(sed -n 's/^ldflags[[:space:]]*=[[:space:]]*//p' "$pkg/ct.pkg" | tr -d '\r')
if [ "${#ldf}" -lt 160 ]; then
    ok "ldflags fits ct's 160-byte metadata cap (${#ldf} bytes)"
else
    bad "ldflags length" "${#ldf} bytes >= 160; ct get reports 'metadata that does not fit'"
fi
if find "$pkg" -type d -name vendor -not -path '*/.git/*' | grep -q .; then
    bad "no vendor/ directory" "ct get skips vendor/, so the dependency would vanish"
else
    ok "no vendor/ directory"
fi
echo

echo "== third_party integrity =="
if (cd "$pkg" && shasum -a 256 -c tools/third_party.sha256) >"$work/sha.log" 2>&1; then
    ok "sha256 manifest"
else
    bad "sha256 manifest" "see log"
    grep -v ': OK$' "$work/sha.log" >&2
fi
manifest=$(awk '{print $2}' "$pkg/tools/third_party.sha256" | sort)
actual=$(cd "$pkg/third_party" && ls -1 | grep -v '^README.md$' | sed 's|^|third_party/|' | sort)
if [ "$manifest" = "$actual" ]; then
    ok "manifest covers every third_party file"
else
    bad "manifest coverage" "manifest and directory disagree"
    diff <(printf '%s\n' "$manifest") <(printf '%s\n' "$actual") >&2
fi
if grep -q 'Redistribution and use in source and binary forms' "$pkg/third_party/COPYING" &&
   grep -q 'THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS' "$pkg/third_party/COPYING" &&
   grep -q 'Neither the name of Redis' "$pkg/third_party/COPYING"; then
    ok "COPYING is the BSD-3-Clause text"
else
    bad "COPYING" "does not look like the Redis BSD-3-Clause text"
fi
if grep -q 'Copyright (c) 2006-2012, Salvatore Sanfilippo' "$pkg/third_party/dict.c"; then
    ok "upstream copyright header kept in dict.c"
else
    bad "copyright header" "dict.c header was modified"
fi
if grep -liE 'RSALv2|Server Side Public License|GNU AFFERO' \
        "$pkg/third_party/dict.c" "$pkg/third_party/dict.h" \
        "$pkg/third_party/siphash.c" "$pkg/third_party/zmalloc.c" \
        "$pkg/third_party/zmalloc.h" "$pkg/third_party/mt19937-64.c" \
        "$pkg/third_party/mt19937-64.h" "$pkg/third_party/COPYING" >"$work/lic.log" 2>&1; then
    bad "licence boundary" "a non-BSD Redis licence appears in third_party/"
else
    ok "licence boundary (7.2.x BSD-3-Clause only)"
fi
echo

echo "== public OO surface =="
if grep -qE '^[[:space:]]*pub[[:space:]]+fn[[:space:]]' "$pkg/dict"/*.ct; then
    bad "no free pub fn" "every public method must have a type host"
else
    ok "no free pub fn"
fi
aliases=$(grep -cE '^pub type Dict(Str|Int)Ptr(Iter)? = struct ct_dict_(str|int)ptr(_iter)?;$' "$pkg/dict/Dict.m.ct")
if [ "$aliases" -eq 4 ]; then
    ok "four pub type aliases to struct ct_dict_*"
else
    bad "pub type aliases" "expected 4, found $aliases"
fi
leaks=$(awk '
    /^pub trait / { intrait = 1; next }
    /^[[:space:]]*}/ { intrait = 0 }
    intrait && /(dict[A-Z_]|dictEntry|dictIterator|dictType|uint64_t|int64_t|size_t)/ { print NR": "$0 }
' "$pkg/dict/Dict.m.ct")
if [ -z "$leaks" ]; then
    ok "no dict/C type in trait signatures"
else
    bad "C type leak" "C names in the CT surface"
    printf '%s\n' "$leaks" >&2
fi
chain=$(grep -cE 'Dict(Str|Int)Ptr \*(put|clear|reserve)\(' "$pkg/dict/Dict.m.ct")
if [ "$chain" -eq 6 ]; then
    ok "put/clear/reserve return the receiver (6 declarations)"
else
    bad "chain return types" "expected 6 receiver-returning declarations, found $chain"
fi
# status() is retained on purpose: it reports DICT_NOT_FOUND / DICT_INVALID only
# and is not an OOM channel. Removing it because OOM aborts would silently drop
# the logical error surface, so pin both declarations and both mappings.
status_decl=$(grep -cE '^[[:space:]]*int status\(\);' "$pkg/dict/Dict.m.ct")
status_map=$(grep -cE 'status\(d\) -> ct_dict_(str|int)ptr_status\(d\);' "$pkg/dict/Dict.m.ct")
if [ "$status_decl" -eq 2 ] && [ "$status_map" -eq 2 ]; then
    ok "status() is retained on both shapes (2 declarations, 2 mappings)"
else
    bad "status() retention" "declarations=$status_decl mappings=$status_map, expected 2 and 2"
fi
awk '
    /^pub trait / { t = $3; sub(/\{.*/, "", t); intrait = 1; next }
    /^[[:space:]]*}/ { intrait = 0 }
    intrait && /;[[:space:]]*$/ {
        line = $0
        if (match(line, /[A-Za-z_][A-Za-z0-9_]*[[:space:]]*\(/)) {
            n = substr(line, RSTART, RLENGTH)
            sub(/[[:space:]]*\($/, "", n)
            print "trait " t " " n
        }
    }
' "$pkg/dict/Dict.m.ct" | sort >"$work/trait.txt"
awk '
    /^map trait / { t = $3; inm = 1; next }
    /^[[:space:]]*}/ { inm = 0 }
    inm && /->/ {
        n = $1; sub(/\(.*/, "", n); print "trait " t " " n
    }
' "$pkg/dict/Dict.m.ct" | sort >"$work/mapped.txt"
if diff -u "$work/trait.txt" "$work/mapped.txt" >"$work/map.diff" 2>&1; then
    ok "every trait method has exactly one mapping"
else
    bad "trait/mapping mismatch" "see diff"
    cat "$work/map.diff" >&2
fi
awk '
    /^map trait / { inm = 1; next }
    /^[[:space:]]*}/ { inm = 0 }
    inm && /->/ {
        line = $0
        sub(/^.*->[[:space:]]*/, "", line)
        sub(/\(.*/, "", line)
        sub(/[[:space:]]+$/, "", line)
        print line
    }
' "$pkg/dict/Dict.m.ct" | sort >"$work/cfuncs.txt"
if uniq -d "$work/cfuncs.txt" | grep -q .; then
    bad "mapping names" "a mapped C function appears twice"
else
    ok "mapped C function names are unique"
fi
echo

echo "== shim =="
# No mutable static state may live in a header: every `.ct` is its own
# translation unit, so a per-header latch (the old `static int ct_dict_seeded`)
# would exist once per TU and the second one would reseed the process-global.
# Mutable configuration tables are not allowed either; the two dictType values
# are immutable and are the only non-inline definitions permitted.
bad_static=$(awk '/^static / && !/^static inline / && !/^static dictType / { print NR": "$0 }' "$pkg/dict/Dict.shim.h")
bad_state=$(grep -nE '^[[:space:]]+static[[:space:]]' "$pkg/dict/Dict.shim.h")
if [ -z "$bad_static" ] && [ -z "$bad_state" ]; then
    ok "no mutable static state in the shim (static inline or immutable dictType only)"
else
    bad "shim state" "non-static-inline definition or function-local static"
    printf '%s\n%s\n' "$bad_static" "$bad_state" >&2
fi
if grep -qE '^void ct_dict_seed_once\(void\);$' "$pkg/dict/Dict.shim.h" &&
   ! grep -qE 'ct_dict_seed_once\(void\)[[:space:]]*\{' "$pkg/dict/Dict.shim.h"; then
    ok "seed is declared, not defined, in the shim"
else
    bad "seed declaration" "shim must declare ct_dict_seed_once and never define it"
fi
if grep -qE '^void ct_dict_seed_once\(void\)$' "$pkg/dict/seed.c" &&
   grep -qE '^[[:space:]]+static int seeded = 0;$' "$pkg/dict/seed.c"; then
    ok "seed.c defines one process-wide latch"
else
    bad "seed unit" "seed.c must define ct_dict_seed_once with a function-local latch"
fi
if grep -q 'dict/seed.c' "$pkg/tests/run.sh"; then
    ok "tests link dict/seed.c"
else
    bad "tests seed link" "tests/run.sh LDFLAGS must name dict/seed.c"
fi
if grep -q 'dictGetSafeIterator' "$pkg/dict/Dict.shim.h" && ! grep -q 'dictGetIterator(' "$pkg/dict/Dict.shim.h"; then
    ok "iterator is a safe iterator"
else
    bad "iterator" "must use dictGetSafeIterator and never dictGetIterator"
fi
if grep -q '#undef DICT_OK' "$pkg/dict/Dict.shim.h" && grep -q '#undef DICT_ERR' "$pkg/dict/Dict.shim.h"; then
    ok "upstream DICT_OK/DICT_ERR macros are undefined before the enum"
else
    bad "status enum collision" "dict.h macros must be undefined in the shim"
fi
echo

echo "== allocator discipline =="
raw=$(grep -nE '(^|[^zA-Za-z_])(malloc|calloc|realloc|free)[[:space:]]*\(' "$pkg/third_party/dict.c")
if [ -z "$raw" ]; then
    ok "dict.c has no raw allocator calls (all go through zmalloc/zfree)"
else
    bad "allocator discipline" "dict.c calls a raw allocator"
    printf '%s\n' "$raw" >&2
fi
echo

echo "== README =="
sections=$(grep -c '^## ' "$pkg/README.md")
if [ "$sections" -eq 10 ]; then
    ok "exactly 10 sections"
else
    bad "README sections" "expected 10, found $sections"
fi
grep '^## ' "$pkg/README.md" | sed 's/^## //' >"$work/sections.txt"
cat >"$work/sections.want" <<'EOF'
1. 定位与边界
2. 依赖与安装
3. 快速开始
4. API 全表
5. 教程正文
6. 常见配方
7. 术语表（Glossary）
8. 排错
9. 限制与未支持
10. 版本与变更记录
EOF
if diff -u "$work/sections.want" "$work/sections.txt" >"$work/sections.diff" 2>&1; then
    ok "section titles and order"
else
    bad "README section order" "see diff"
    cat "$work/sections.diff" >&2
fi
if grep -q '^| 术语 | 含义 | 本包中的所指 |$' "$pkg/README.md"; then
    ok "glossary is the three-column table"
else
    bad "glossary table" "header must be | 术语 | 含义 | 本包中的所指 |"
fi
for code in DICT_OK DICT_NOT_FOUND DICT_INVALID; do
    if grep -q "\`$code\`" "$pkg/README.md"; then
        ok "README names $code"
    else
        bad "README $code" "status code is undocumented"
    fi
done
# C type names must not appear on the copy-paste surface: README examples, API
# tables, demo and CT tests. The glossary is exempt: its declared purpose is to
# spell out the handle / dictType / allocator identity mapping, so the C names
# there are the subject, not a token a reader would copy. Prose references such
# as "Redis `zmalloc`" and file names such as `dict.c` are not declarations and
# are deliberately not matched.
leak_re='dictEntry|dictIterator|dictType|uint64_t|int64_t|(^|[^A-Za-z_])size_t([^A-Za-z_]|$)|struct[[:space:]]+ct_dict_|dict[[:space:]]*\*|dictGet[A-Za-z]*\(|dict(Add|Replace|Delete|Find)\(|zmalloc\(|zfree\('
leaks="$(
    {
        awk '
            /^## / { glossary = ($0 ~ /术语表/ || $0 ~ /Glossary/) ; next }
            !glossary { print FILENAME ":" FNR "|" $0 }
        ' "$pkg/README.md"
        grep -rEn "$leak_re" "$pkg/demo"/*.ct "$pkg/tests"/*.ct 2>/dev/null || true
    } | grep -E "$leak_re" || true
)"
if [ -z "$leaks" ]; then
    ok "no C types in README examples / API docs / demo / CT tests"
else
    bad "README C type leak" "see lines"
    printf '%s\n' "$leaks" | head -5 >&2
fi
if grep -q '未发布' "$pkg/README.md" && grep -q 'third_party/' "$pkg/README.md"; then
    ok "README states the unpublished status and the third_party layout"
else
    bad "README honesty" "must state the unpublished status and third_party/"
fi
echo

echo "== README ct blocks =="
PKG_CFLAGS="-I $pkg/third_party -Wall -Wextra"
PKG_LDFLAGS="$pkg/third_party/dict.c $pkg/third_party/siphash.c $pkg/third_party/zmalloc.c $pkg/third_party/mt19937-64.c $pkg/dict/seed.c"
rm -f "$work"/readme-*.ct "$work"/readme-*.out
# Every ```ct block in the README must be a complete program. Extract, build,
# run, and compare stdout: prose that does not compile is a documentation bug,
# and a code block that was never built cannot be trusted.
awk -v out="$work" '
    /^```ct$/ { n++; inblock = 1; next }
    inblock && /^```$/ { inblock = 0; next }
    inblock { print > (out "/readme-" n ".ct") }
' "$pkg/README.md"
printf '0\n2\n' >"$work/readme-1.want"
printf 'len: 3\ntwo: 2\nlen: 2\nclear returns receiver: 1\n' >"$work/readme-2.want"
blocks=0
for src in "$work"/readme-*.ct; do
    [ -e "$src" ] || continue
    n=${src##*/readme-}; n=${n%.ct}
    blocks=$((blocks + 1))
    out="$work/readme-$n"
    log="$work/readme-$n.build.log"
    if ! (cd "$work" && "$CT" build -I "$pkg" --cflags "$PKG_CFLAGS" \
            --ldflags "$PKG_LDFLAGS" "$src" -o "$out") >"$log" 2>&1; then
        bad "README block $n builds" "see log"
        sed -n '1,30p' "$log" >&2
        continue
    fi
    if grep -q 'warning:' "$log"; then
        bad "README block $n builds" "build emitted warnings"
        grep 'warning:' "$log" >&2
        continue
    fi
    if ! "$out" >"$work/readme-$n.out" 2>&1; then
        bad "README block $n runs" "non-zero exit"
        sed -n '1,30p' "$work/readme-$n.out" >&2
        continue
    fi
    if diff -u "$work/readme-$n.want" "$work/readme-$n.out" >"$work/readme-$n.diff" 2>&1; then
        ok "README block $n builds, runs and matches its output"
    else
        bad "README block $n output" "see diff"
        sed -n '1,30p' "$work/readme-$n.diff" >&2
    fi
done
if [ "$blocks" -eq 2 ]; then
    ok "README has 2 complete ct blocks"
else
    bad "README ct blocks" "expected 2 complete ct blocks, found $blocks"
fi
echo

echo "== installed layout (real ct get + ct install) =="
if ! command -v "$CT" >/dev/null 2>&1 && [ ! -x "$CT" ]; then
    bad "installed build" "ct not found: $CT"
else
    printf 'name = dictconsumer\nversion = 0.1.0\nlicence = BSD-3-Clause\n' >"$work/ct.pkg"
    install_ok=1
    if ! (cd "$work" && "$CT" get "$pkg") >"$work/get.log" 2>&1; then
        bad "ct get" "see log"
        cat "$work/get.log" >&2
        install_ok=0
    elif [ ! -f "$work/vendor/dict/third_party/dict.c" ] ||
         [ ! -f "$work/vendor/dict/third_party/zmalloc.h" ]; then
        bad "ct get copy" "third_party/ did not survive the install copy"
        ls -R "$work/vendor/dict" >&2
        install_ok=0
    else
        ok "ct get copies third_party/ into vendor/dict"
    fi
    if [ "$install_ok" -eq 1 ]; then
        if ! (cd "$work" && "$CT" install) >"$work/install.log" 2>&1; then
            bad "ct install" "see log"
            cat "$work/install.log" >&2
            install_ok=0
        elif grep -q 'vendor/dict/third_party' "$work/install.log" &&
             grep -q 'vendor/dict/dict/seed.c' "$work/install.log"; then
            ok "ct install prints the third_party and seed flags"
        else
            bad "ct install flags" "third_party missing from the printed command"
            cat "$work/install.log" >&2
            install_ok=0
        fi
    fi
    if [ "$install_ok" -eq 1 ]; then
        (
            cd "$work" || exit 1
            "$CT" build -I vendor/dict \
                --cflags "-I vendor/dict/third_party -Wall -Wextra" \
                --ldflags "vendor/dict/third_party/dict.c vendor/dict/third_party/siphash.c vendor/dict/third_party/zmalloc.c vendor/dict/third_party/mt19937-64.c vendor/dict/dict/seed.c" \
                vendor/dict/demo/main.ct -o demo_installed
        ) >"$work/installed.log" 2>&1
        if [ $? -eq 0 ] && ! grep -q 'warning:' "$work/installed.log"; then
            if "$work/demo_installed" >"$work/installed.out" 2>&1 &&
               diff -u "$pkg/demo/expected.out" "$work/installed.out" >"$work/installed.diff" 2>&1; then
                ok "install-layout build, run and output"
            else
                bad "install-layout run" "see diff"
                cat "$work/installed.diff" >&2
            fi
        else
            bad "install-layout build" "see log"
            cat "$work/installed.log" >&2
        fi
    fi
fi
echo

echo "== tests/run.sh =="
if (cd "$pkg" && CT="$CT" ./tests/run.sh) >"$work/tests.log" 2>&1; then
    ok "tests/run.sh"
else
    bad "tests/run.sh" "see log"
    cat "$work/tests.log" >&2
fi
echo

printf '%s checks, %s failure(s)\n' "$((pass + fail))" "$fail"
[ "$fail" -eq 0 ]
