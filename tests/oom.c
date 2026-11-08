/*
 * oom.c - the OOM contract, proven by injecting an allocation failure rather
 * than by asserting it in prose.
 *
 * The test binary defines `malloc`, so the `malloc` calls compiled into
 * zmalloc.c bind to this file's definition (a symbol in the main executable
 * wins over libc for references from the executable's own objects). Arming
 * `fail_malloc` makes the next allocation return NULL, exactly what a real OOM
 * looks like to zmalloc.
 *
 * Four children:
 *
 *   1. arm, then put()            -> zmalloc's OOM handler runs and the process
 *                                    dies with SIGABRT, printing the fixture
 *                                    marker. This is the documented "OOM is
 *                                    process termination, not an error code".
 *   2. arm, then reserve()        -> the shim has already ruled out logical
 *                                    refusals, so dictTryExpand's failure is a
 *                                    real allocation failure: the same OOM
 *                                    handler runs and the process aborts.
 *   3. arm, then construct a table-> the handle allocation itself aborts too:
 *                                    there is no partial handle to leak.
 *   4. reserve() with no arm but a logical refusal (same power-of-two) -> no
 *                                    abort, status() is CT_DICT_INVALID. This
 *                                    proves the two causes are separated.
 *
 * Exit code 0 means all four behaved; the parent prints
 * "OOM passed=4 failed=0".
 *
 * License: BSD-3-Clause (see LICENSE).
 */

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "Dict.shim.h"

static int fail_malloc = 0;

#if defined(__APPLE__)
#include <malloc/malloc.h>
void *malloc(size_t size)
{
    if (fail_malloc) {
        return NULL;
    }
    return malloc_zone_malloc(malloc_default_zone(), size);
}

void *calloc(size_t count, size_t size)
{
    if (fail_malloc) {
        return NULL;
    }
    return malloc_zone_calloc(malloc_default_zone(), count, size);
}

void *realloc(void *ptr, size_t size)
{
    if (fail_malloc) {
        return NULL;
    }
    return malloc_zone_realloc(malloc_default_zone(), ptr, size);
}
#else
#include <malloc.h>
void *malloc(size_t size)
{
    if (fail_malloc) {
        return NULL;
    }
    return __libc_malloc(size);
}

void *calloc(size_t count, size_t size)
{
    if (fail_malloc) {
        return NULL;
    }
    return __libc_calloc(count, size);
}

void *realloc(void *ptr, size_t size)
{
    if (fail_malloc) {
        return NULL;
    }
    return __libc_realloc(ptr, size);
}
#endif

static void oom_marker(size_t size)
{
    static const char msg[] = "CT_DICT_OOM\n";

    (void)size;
    (void)write(2, msg, sizeof(msg) - 1);
    abort();
}

/*
 * Runs `body` in a child whose stderr is captured, and reports how it ended.
 * Returns 1 on success. `want_signal` selects the expected end.
 */
static int run_child(void (*body)(void), int want_signal, int want_code,
                     const char *marker, const char *name)
{
    int fds[2];
    pid_t pid;
    int status;
    char buf[4096];
    ssize_t n;
    size_t used;

    if (pipe(fds) != 0) {
        printf("FAIL %s: pipe\n", name);
        return 0;
    }
    pid = fork();
    if (pid < 0) {
        printf("FAIL %s: fork\n", name);
        return 0;
    }
    if (pid == 0) {
        close(fds[0]);
        dup2(fds[1], 2);
        close(fds[1]);
        body();
        _exit(0);
    }

    close(fds[1]);
    used = 0;
    while ((n = read(fds[0], buf + used, sizeof(buf) - 1 - used)) > 0) {
        used = (size_t)n + used;
        if (used == sizeof(buf) - 1) {
            break;
        }
    }
    buf[used] = 0;
    close(fds[0]);

    if (waitpid(pid, &status, 0) < 0) {
        printf("FAIL %s: waitpid\n", name);
        return 0;
    }

    if (want_signal) {
        if (!WIFSIGNALED(status) || WTERMSIG(status) != want_signal) {
            printf("FAIL %s: expected signal %d, status 0x%x\n", name, want_signal, status);
            return 0;
        }
        if (marker && strstr(buf, marker) == NULL) {
            printf("FAIL %s: marker missing from stderr: %s\n", name, buf);
            return 0;
        }
        return 1;
    }

    if (!WIFEXITED(status) || WEXITSTATUS(status) != want_code) {
        printf("FAIL %s: expected exit %d, status 0x%x\n", name, want_code, status);
        return 0;
    }
    return 1;
}

static void child_put_aborts(void)
{
    ct_dict_intptr *h = ct_dict_intptr_new();
    zmalloc_set_oom_handler(oom_marker);
    fail_malloc = 1;
    ct_dict_intptr_put(h, 42, 0);
    _exit(0);
}

static void child_reserve_aborts(void)
{
    ct_dict_intptr *h = ct_dict_intptr_new();
    ct_dict_intptr_reserve(h, 64);
    zmalloc_set_oom_handler(oom_marker);
    fail_malloc = 1;
    ct_dict_intptr_reserve(h, 1024);
    _exit(0);
}

static void child_reserve_logical_refusal(void)
{
    ct_dict_intptr *h = ct_dict_intptr_new();
    zmalloc_set_oom_handler(oom_marker);
    ct_dict_intptr_reserve(h, 64);
    ct_dict_intptr_reserve(h, 64);
    _exit(ct_dict_intptr_status(h) == CT_DICT_INVALID ? 7 : 8);
}

static void child_new_aborts(void)
{
    zmalloc_set_oom_handler(oom_marker);
    fail_malloc = 1;
    (void)ct_dict_intptr_new();
    _exit(0);
}

int main(void)
{
    int passed = 0;
    int failed = 0;

    if (run_child(child_put_aborts, SIGABRT, 0, "CT_DICT_OOM", "put OOM aborts")) {
        passed = passed + 1;
    } else {
        failed = failed + 1;
    }
    if (run_child(child_reserve_aborts, SIGABRT, 0, "CT_DICT_OOM", "reserve OOM aborts")) {
        passed = passed + 1;
    } else {
        failed = failed + 1;
    }
    if (run_child(child_new_aborts, SIGABRT, 0, "CT_DICT_OOM", "constructor OOM aborts")) {
        passed = passed + 1;
    } else {
        failed = failed + 1;
    }
    if (run_child(child_reserve_logical_refusal, 0, 7, NULL, "reserve logical refusal does not abort")) {
        passed = passed + 1;
    } else {
        failed = failed + 1;
    }

    printf("OOM passed=%d failed=%d\n", passed, failed);
    return failed != 0;
}
