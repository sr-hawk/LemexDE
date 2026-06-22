/*
 * build.h — the build pipeline: a forward pass of content-addressed phases.
 *
 *   sources -> compile(each file) -> link -> product
 *
 * Each compile is keyed by the content hash of the file; the object file is
 * named after that hash, so its mere existence on disk *is* the cache — an
 * unchanged file is never recompiled (the same idea git uses for blobs). The
 * compiler's diagnostics are parsed into structured records, which is the
 * agent's / editor's primary feedback signal.
 *
 * The phase that shells out to the compiler dispatches by extension (.c -> cc,
 * .swift -> swiftc, ...). C is fully exercised here with the host compiler; the
 * Swift path is the same shape but needs the on-device toolchain to run.
 */
#ifndef FORGE_BUILD_H
#define FORGE_BUILD_H

#include "ide.h"
#include <stddef.h>

typedef enum { DIAG_NOTE, DIAG_WARNING, DIAG_ERROR } DiagSeverity;

typedef struct {
    char        *file;       /* source path the compiler reported */
    int          line, col;  /* 1-based; 0 if absent */
    DiagSeverity severity;
    char        *message;
} Diagnostic;

typedef struct {
    int          ok;            /* compiled with no errors AND linked */
    Diagnostic  *diags;
    size_t       diag_count;
    int          error_count, warning_count;
    int          compiled_count, cached_count; /* phases run vs. reused from cache */
    char        *product;       /* path to the linked executable, or NULL */
} BuildResult;

/* The compiler/linker invocation seam — the one part of the pipeline that is
 * platform-specific. `exec` runs a tool with a NULL-terminated argv, captures
 * merged stdout+stderr (returns it; caller frees), and sets *exit_code; `ctx` is
 * passed through. The POSIX default shells out to the host compiler (what makes
 * `swift run` + the tests work on macOS). On iOS you supply an `exec` that drives
 * forge's *embedded* clang/lld (LLVM-On-iOS) IN-PROCESS — no subprocess, no other
 * app; forge owns the compiler. Either way the content-addressing / diagnostics /
 * linking pipeline is reused unchanged. argv carries pure tool arguments;
 * capturing output is the exec impl's job (no shell assumptions leak in). */
typedef struct {
    char *(*exec)(const char *const *argv, int *exit_code, void *ctx);
    void  *ctx;
} ForgeToolchain;

/* A build's full configuration: which toolchain, and what kind of product.
 * `shared` links a dynamic library instead of an executable (and compiles -fPIC).
 * That's the iOS path: there is no exec, so the product is a dylib the runner
 * dlopen's. Executables (shared=0) are the host/fork-exec path. */
typedef struct {
    ForgeToolchain toolchain;
    int            shared;
} ForgeBuildConfig;

/* Compile every present source file under `workdir` (created if needed),
 * content-addressing objects, parse diagnostics, then link. The `_view` form
 * builds either trunk or a branch through one read interface — so an agent can
 * compile its speculative branch with no separate pipeline. The content hash of
 * each file keys the object cache, so a branch whose files match trunk links
 * for free (every compile is a cache hit). The `_tc` form takes an explicit
 * toolchain; the others use the POSIX default. */
BuildResult build_project_cfg(IdeView view, const char *workdir, ForgeBuildConfig cfg);
BuildResult build_project_tc(IdeView view, const char *workdir, ForgeToolchain tc); /* executable */
BuildResult build_project_view(IdeView view, const char *workdir);   /* POSIX, executable */
BuildResult build_project(IdeCore *core, const char *workdir);       /* trunk + POSIX, executable */
BuildResult build_project_shared(IdeView view, const char *workdir); /* POSIX, dylib (dlopen-run) */
void        build_result_free(BuildResult *r);

/* The default host toolchain (shells out to cc). The op kernel's meta-ops reuse
 * this to compile a proposed op; on iOS pass forge's embedded-clang toolchain. */
ForgeToolchain forge_posix_toolchain(void);

#endif /* FORGE_BUILD_H */
