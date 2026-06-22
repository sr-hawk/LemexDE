/*
 * ops.h — forge's operation kernel: one typed, self-describing op set.
 *
 * The lesson from sh3d: when every mutation is a strict single idempotent
 * forward pass, a *universal op kernel* falls out for free — a closed set of
 * named, typed ops, one deterministic applier, a uniform result envelope, and
 * a machine-readable schema derived from the op table itself. That makes the
 * system extremely modular: add a capability = add one table row; the
 * dispatcher and the schema update themselves.
 *
 * forge's core (ide.h) is already that forward pass at the *primitive* level
 * (insert/delete/create/rename are complete for text). This layer is the level
 * above: the verbs a human UI and an AI agent both invoke. Both clients emit
 * the same envelope — `(op name, args)` — and `forge_op_apply` is the single
 * execution path, exactly as sh3d's `sh3d apply` is for its 67 ops.
 *
 * Design choices mirrored from sh3d:
 *   - One result envelope (`OpResult`: ok + reason + detail), never bespoke.
 *   - A registry (`forge_op_table`) the dispatcher AND `forge_schema` derive from.
 *   - Args are a uniform bag read via typed helpers, so the dispatch is fully
 *     table-driven and a JSON front-end is a thin layer over `OpArgs` later.
 *   - Transactions are forge's speculative branches (fork/merge/discard).
 *
 * Pure C, no JSON dependency: callers build `OpArgs` programmatically (the
 * Swift agent, the touch UI, or a future JSON parser all target this).
 */
#ifndef FORGE_OPS_H
#define FORGE_OPS_H

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include "ide.h"
#include "build.h"   /* ForgeToolchain — meta-ops compile through the build seam */

/* ---------- result envelope (sh3d's OpResult, forge's reasons) ---------- */

typedef enum {
    OP_OK = 0,
    /* bad request */
    OP_ERR_NO_SUCH_OP, OP_ERR_BAD_ARG, OP_ERR_MISSING_ARG,
    /* missing target */
    OP_ERR_NO_SUCH_FILE, OP_ERR_FILE_ABSENT, OP_ERR_NO_SUCH_BRANCH,
    /* bounds / state */
    OP_ERR_BAD_OFFSET, OP_ERR_BAD_RANGE, OP_ERR_NOTHING_TO_DO,
    /* pipeline */
    OP_ERR_BUILD_FAILED, OP_ERR_RUN_FAILED,
    /* internal */
    OP_ERR_INTERNAL,
} OpReason;

typedef struct {
    int      ok;            /* 1 = success */
    OpReason reason;        /* OP_OK on success */
    long     value;         /* single-value return: new FileId / BranchId / count */
    char     detail[128];   /* human/agent-readable explanation */
} OpResult;

const char *op_reason_name(OpReason r);   /* stable token, e.g. "bad_offset" */

/* Public result constructors. A meta-op-authored op is compiled separately, so
 * it needs these (and op_arg_*) in the ABI to read args and return a result. */
OpResult op_result_ok(long value);
OpResult op_result_error(OpReason reason, const char *detail);

/* ---------- argument bag (the uniform envelope payload) ---------- */

typedef struct OpArgs OpArgs;

OpArgs *op_args_new(void);
void    op_args_free(OpArgs *a);
void    op_args_set_str(OpArgs *a, const char *key, const char *val);
void    op_args_set_int(OpArgs *a, const char *key, long val);

/* Typed readers. `*found` (nullable) reports presence so an op can tell
 * "absent" from "present-but-zero". */
const char *op_arg_str(const OpArgs *a, const char *key, const char *dflt);
long        op_arg_int(const OpArgs *a, const char *key, long dflt, int *found);
int         op_arg_has(const OpArgs *a, const char *key);

/* ---------- op context (non-arg ambient inputs) ---------- */

typedef struct {
    const char *workdir;   /* build/run scratch dir; may be NULL */
    FILE       *out;       /* where query ops (inspect/list/schema) write; may be NULL */
} OpContext;

/* ---------- op descriptor + registry ---------- */

typedef OpResult (*OpFn)(IdeCore *core, const OpArgs *args, OpContext *ctx);

typedef struct {
    const char *name;      /* "insert_text" */
    const char *arg_sig;   /* "file:int, offset:int, text:str, [branch:int]" */
    const char *doc;       /* one line */
    int         mutates;   /* 1 = logged state change; 0 = query/pipeline */
    OpFn        fn;
} OpDef;

/* The table the dispatcher and schema both derive from (NULL-name terminated). */
const OpDef *forge_op_table(size_t *count);
const OpDef *forge_op_find(const char *name);

/* The single execution path. Looks up `op_name`, validates, applies. */
OpResult forge_op_apply(IdeCore *core, const char *op_name,
                        const OpArgs *args, OpContext *ctx);

/* Emit the machine-readable op contract (one line per op: name, sig, doc) —
 * what an LLM driver caches at session start, the analog of `sh3d schema`.
 * Lists built-in AND dynamically-accepted (meta-op-authored) ops. */
void forge_schema(FILE *out);

/* List the op archetypes (skeletons that encode the forward-pass discipline so a
 * new op is authored by filling one <<BODY>> blank). See propose_op's `archetype`. */
void forge_archetypes(FILE *out);

/* find_op — BM25 retrieval over the op set by intent (each op's name+arg_sig+doc is
 * its breadcrumb). Keeps the kernel navigable for a small model as it grows: search,
 * don't enumerate. Fills `out` with up to `max` ranked hits; returns the count. */
typedef struct { const char *name; double score; } OpHit;
size_t forge_find_op(const char *intent, OpHit *out, size_t max);

/* Four projections of the one op table — derived, never hand-maintained, so they
 * can't drift. Two constrain generation (per-intent, narrowed by find_op); two
 * describe the whole surface for reading. */

/* GBNF grammar — hard generation constraint for an on-device (llama.cpp) model. */
size_t forge_grammar_for_intent(const char *intent, size_t k, FILE *out);

/* JSON Schema — generation constraint for a structured-output / strict-tool API
 * (e.g. Claude). Accepts only { "op": <one of top-k>, "args": { ...typed... } }. */
size_t forge_jsonschema_for_intent(const char *intent, size_t k, FILE *out);

/* Human-readable catalog (Markdown) — the whole op set, grouped, with args,
 * examples, and reversibility. The map a person browses. */
void forge_catalog(FILE *out);

/* Plain-text, reader-agnostic schema — a stable, line-oriented contract any
 * reader (agent, tool, script, human) can parse without a JSON/grammar engine. */
void forge_schema_text(FILE *out);

/* ===================================================================== *
 * Meta-ops — forge grows its own op set at runtime (sh3d's standout move,
 * made genuinely LIVE by forge's in-process compile + dlopen):
 *
 *   propose_op(name, arg_sig, doc, body) — writes the op's C source
 *   test_op(name)    — compiles it to a dylib via the toolchain; on failure
 *                      returns the compiler diagnostics (no crash)
 *   accept_op(name)  — dlopen + register it in the live table; callable
 *                      immediately through forge_op_apply, no relaunch
 *   reject_op(name)  — drop the proposal
 *
 * These are ordinary table ops (drive them via forge_op_apply). `body` is the
 * function body of:
 *     OpResult forge_dynop(IdeCore *core, const OpArgs *args, OpContext *ctx)
 * so it reads args (op_arg_*), drives the core (ide_*), and returns via
 * op_result_ok / op_result_error.
 *
 * Configure the compile seam once before using them. `scratch_dir` holds the
 * generated sources/dylibs; `include_dir` is where ops.h/ide.h live (for -I).
 * On host pass forge_posix_toolchain(); on iOS pass forge's embedded clang. The
 * host executable must export its symbols (link with -rdynamic) so an accepted
 * op resolves the op_arg, ide, and op_result functions from the main image at
 * dlopen time. */
void forge_meta_configure(ForgeToolchain toolchain,
                          const char *scratch_dir, const char *include_dir);

#endif /* FORGE_OPS_H */
