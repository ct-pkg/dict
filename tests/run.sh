#!/usr/bin/env bash
#
# tests/run.sh - build every dict suite with -Wall -Wextra, run it, and require
# a warning-free build, an all-pass summary, and at least the measured number of
# assertions per suite (so gutting check() cannot silently pass).
#
#   ./tests/run.sh
#   CT=/path/to/ct ./tests/run.sh
#
# Exit code 0 means: four CT suites passed, the two C gates passed, and the
# demo output matches demo/expected.out byte for byte.
#
# Builds happen in a temporary directory, so the package tree stays clean.
#
set -u

PKG=$(cd "$(dirname "$0")/.." && pwd)
CT=${CT:-/Users/samxx/.local/bin/ct}
CC=${CC:-cc}
WORK=$(mktemp -d "${TMPDIR:-/tmp}/ct-dict-tests.XXXXXX")
trap 'rm -rf "$WORK"' EXIT

LDFLAGS="$PKG/third_party/dict.c $PKG/third_party/siphash.c $PKG/third_party/zmalloc.c $PKG/third_party/mt19937-64.c $PKG/dict/seed.c"
CFLAGS="-I $PKG/third_party -Wall -Wextra"

failures=0
suites=0

note_fail() {
    printf 'FAIL %s\n' "$1"
    failures=$((failures + 1))
}

# Build one CT source and run it; require no warnings, SUMMARY failed=0, and at
# least `min` passed assertions. The floor is the measured count: gutting a
# suite's check() so it reports "passed=2 failed=0" turns this red.
run_ct_suite() {
    local name=$1
    local src=$2
    local min=$3
    local out="$WORK/$name"
    local log="$WORK/$name.build.log"

    suites=$((suites + 1))
    if ! (cd "$WORK" && "$CT" build -I "$PKG" --cflags "$CFLAGS" --ldflags "$LDFLAGS" "$src" -o "$out") >"$log" 2>&1; then
        note_fail "$name: build failed"
        sed -n '1,40p' "$log"
        return
    fi
    if grep -q 'warning:' "$log"; then
        note_fail "$name: build emitted warnings"
        grep 'warning:' "$log"
        return
    fi
    if ! "$out" >"$WORK/$name.out" 2>&1; then
        note_fail "$name: non-zero exit"
        sed -n '1,40p' "$WORK/$name.out"
        return
    fi
    if ! grep -q '^SUMMARY passed=[0-9][0-9]* failed=0$' "$WORK/$name.out"; then
        note_fail "$name: missing an all-pass SUMMARY"
        sed -n '1,40p' "$WORK/$name.out"
        return
    fi
    local passed
    passed=$(sed -n 's/^SUMMARY passed=\([0-9][0-9]*\) failed=0$/\1/p' "$WORK/$name.out" | tail -n 1)
    if [ "$passed" -lt "$min" ]; then
        note_fail "$name: expected at least $min assertions, got $passed"
        return
    fi
    printf 'ok   %s (%s)\n' "$name" "$(tail -n 1 "$WORK/$name.out")"
}

# Build one C gate; require no warnings, a zero exit, and at least `min`
# passing assertions from its own summary line.
run_c_gate() {
    local name=$1
    local src=$2
    local min=$3
    local out="$WORK/$name"
    local log="$WORK/$name.build.log"

    suites=$((suites + 1))
    if ! (cd "$PKG" && "$CC" -std=c11 -Wall -Wextra -I "$PKG/dict" -I "$PKG/third_party" \
            "$src" $LDFLAGS -o "$out") >"$log" 2>&1; then
        note_fail "$name: build failed"
        sed -n '1,40p' "$log"
        return
    fi
    if grep -q 'warning:' "$log"; then
        note_fail "$name: build emitted warnings"
        grep 'warning:' "$log"
        return
    fi
    if ! "$out" >"$WORK/$name.out" 2>&1; then
        note_fail "$name: non-zero exit"
        sed -n '1,40p' "$WORK/$name.out"
        return
    fi
    local summary passed
    summary=$(grep -E '^[A-Z]+ passed=[0-9]+ failed=0$' "$WORK/$name.out" | tail -n 1)
    if [ -z "$summary" ]; then
        note_fail "$name: missing an all-pass assertion summary"
        sed -n '1,40p' "$WORK/$name.out"
        return
    fi
    passed=$(printf '%s\n' "$summary" | sed -n 's/.*passed=\([0-9][0-9]*\) failed=0$/\1/p')
    if [ "$passed" -lt "$min" ]; then
        note_fail "$name: expected at least $min assertions, got $passed"
        return
    fi
    printf 'ok   %s (%s)\n' "$name" "$summary"
}

# Build the generation-guard test with AddressSanitizer: clearing or releasing
# a table while an iterator is alive must not read freed memory.
run_asan_gate() {
    local name=$1
    local src=$2
    local min=$3
    local out="$WORK/$name"
    local log="$WORK/$name.build.log"

    suites=$((suites + 1))
    if ! (cd "$PKG" && "$CC" -std=c11 -Wall -Wextra -g -fsanitize=address \
            -I "$PKG/dict" -I "$PKG/third_party" "$src" $LDFLAGS -o "$out") >"$log" 2>&1; then
        note_fail "$name: ASan build failed"
        sed -n '1,40p' "$log"
        return
    fi
    if grep -q 'warning:' "$log"; then
        note_fail "$name: build emitted warnings"
        grep 'warning:' "$log"
        return
    fi
    if ! ASAN_OPTIONS=detect_leaks=0 "$out" >"$WORK/$name.out" 2>&1; then
        note_fail "$name: ASan reported a problem"
        sed -n '1,60p' "$WORK/$name.out"
        return
    fi
    local summary passed
    summary=$(grep -E '^[A-Z]+ passed=[0-9]+ failed=0$' "$WORK/$name.out" | tail -n 1)
    if [ -z "$summary" ]; then
        note_fail "$name: missing an all-pass assertion summary"
        sed -n '1,40p' "$WORK/$name.out"
        return
    fi
    passed=$(printf '%s\n' "$summary" | sed -n 's/.*passed=\([0-9][0-9]*\) failed=0$/\1/p')
    if [ "$passed" -lt "$min" ]; then
        note_fail "$name: expected at least $min assertions, got $passed"
        return
    fi
    printf 'ok   %s (%s)\n' "$name" "$summary"
}

# Two translation units each include the shim and build a table. The first
# one inserts eight keys, the second constructs another table (which used to
# reseed the process-global from its own per-TU latch), then the first table is
# read back. Repeating catches the old bug's same-second luck.
run_multi_tu() {
    local log="$WORK/multitu.build.log"
    local out="$WORK/multitu"
    local i

    suites=$((suites + 1))
    if ! (cd "$PKG" && "$CC" -std=c11 -Wall -Wextra -I "$PKG/dict" -I "$PKG/third_party" \
            "$PKG/tests/multi_tu_a.c" "$PKG/tests/multi_tu_b.c" $LDFLAGS -o "$out") >"$log" 2>&1; then
        note_fail "multi_tu: build failed"
        sed -n '1,40p' "$log"
        return
    fi
    if grep -q 'warning:' "$log"; then
        note_fail "multi_tu: build emitted warnings"
        grep 'warning:' "$log"
        return
    fi
    for i in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20; do
        if ! "$out" >"$WORK/multitu.out" 2>&1; then
            note_fail "multi_tu: run $i lost keys across translation units"
            sed -n '1,10p' "$WORK/multitu.out"
            return
        fi
    done
    printf 'ok   multi_tu (20 runs)\n'
}

# Build and run the demo, then diff against the checked-in output.
run_demo() {
    local log="$WORK/demo.build.log"
    local out="$WORK/demo"

    suites=$((suites + 1))
    if ! (cd "$WORK" && "$CT" build -I "$PKG" --cflags "$CFLAGS" --ldflags "$LDFLAGS" \
            "$PKG/demo/main.ct" -o "$out") >"$log" 2>&1; then
        note_fail "demo: build failed"
        sed -n '1,40p' "$log"
        return
    fi
    if grep -q 'warning:' "$log"; then
        note_fail "demo: build emitted warnings"
        grep 'warning:' "$log"
        return
    fi
    if ! "$out" >"$WORK/demo.out" 2>&1; then
        note_fail "demo: non-zero exit"
        return
    fi
    if ! diff -u "$PKG/demo/expected.out" "$WORK/demo.out" >"$WORK/demo.diff" 2>&1; then
        note_fail "demo: output differs from demo/expected.out"
        sed -n '1,40p' "$WORK/demo.diff"
        return
    fi
    printf 'ok   demo\n'
}

if [ ! -x "$CT" ]; then
    printf 'FAIL ct not executable: %s\n' "$CT"
    exit 1
fi

run_ct_suite unit "$PKG/tests/unit.ct" 68
run_ct_suite iterator "$PKG/tests/iterator.ct" 343
run_ct_suite rehash "$PKG/tests/rehash.ct" 16
run_ct_suite chain "$PKG/tests/chain.ct" 17
run_c_gate slots "$PKG/tests/slots.c" 1536
run_c_gate oom "$PKG/tests/oom.c" 4
run_asan_gate gen "$PKG/tests/gen.c" 8
run_multi_tu
run_demo

printf '\n%s suites, %s failure(s)\n' "$suites" "$failures"
[ "$failures" -eq 0 ]
