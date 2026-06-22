/*
 * forge_toolchain_stub.c — in-app stubs for ops.c's only host-toolchain symbols.
 *
 * ops.c's `build` op (op_build) references build_project_view + build_result_free
 * from forge's POSIX build pipeline (build.c). That pipeline uses fork/exec/system,
 * which iOS forbids, so build.c is NOT compiled into the app. In the folded app,
 * forge's build/run path routes through emexDE's MDK toolchain (Stage 1); until
 * that's wired, these stubs satisfy the linker. The `build` op simply returns a
 * non-ok result if invoked before Stage 1.
 */
#include "ide.h"
#include "build.h"
#include <string.h>

BuildResult build_project_view(IdeView view, const char *workdir) {
    (void)view; (void)workdir;
    BuildResult r;
    memset(&r, 0, sizeof r);
    r.ok = 0;   /* no host toolchain in-app; routed to emexDE MDK in Stage 1 */
    return r;
}

void build_result_free(BuildResult *r) { (void)r; }
