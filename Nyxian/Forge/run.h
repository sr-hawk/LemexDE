/*
 * run.h — execute a built product, capture its output, bound its time.
 *
 * The portable contract is "run the thing build_project produced, give me back
 * its merged stdout+stderr, its exit code, and whether it had to be killed for
 * running too long." The implementation here is POSIX fork/exec (verifiable on
 * any Unix). On iOS, where you cannot exec an arbitrary binary, the *same*
 * interface is satisfied differently: dlopen the product (a dylib) and call its
 * entry, or drive it through emexDE's LiveProcess. That platform seam is the
 * only place this layer would borrow emexDE.
 */
#ifndef FORGE_RUN_H
#define FORGE_RUN_H

#include <stddef.h>

typedef struct {
    int    exited;       /* 1 if it exited on its own (vs. killed) */
    int    exit_code;    /* valid when exited */
    int    timed_out;    /* killed because it ran past the timeout */
    char  *output;       /* captured stdout+stderr (merged) */
    size_t output_len;
} RunResult;

/* POSIX fork/exec: the product is an executable, run in a child process — fully
 * isolated and killable. The macOS / host path. */
RunResult run_product(const char *path, int timeout_seconds);

/* In-process dlopen: the product is a *dylib*; load it, redirect its stdout+
 * stderr to a pipe, and call its `main` on a worker thread. This is the iOS path
 * (iOS forbids exec of a fresh image). It is portable — dlopen/pipe/dup2/pthread
 * are POSIX — so it's verified on the host too.
 *
 * LIMITATIONS (inherent to running in forge's own process): a program that calls
 * exit() terminates forge; a crash takes forge down; and a runaway can be timed
 * out but NOT safely killed (the worker thread is detached and leaks until the
 * app exits). A robust, killable runner needs process isolation (emexDE's
 * LiveProcess model) — the stage-2b follow-up. */
RunResult run_product_dylib(const char *path, int timeout_seconds);

void      run_result_free(RunResult *r);

#endif /* FORGE_RUN_H */
