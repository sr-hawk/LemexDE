/* ops.c — forge's operation kernel. See ops.h.
 *
 * Every op wraps verbs that already exist in the verified core (ide_*, build_*)
 * behind one envelope. Branch-awareness is a single optional `branch` arg, so
 * trunk and sandbox edits are the *same* op (sh3d's "collapse variants into one
 * op with optional args"). Validation returns structured OpResults instead of
 * tripping the core's asserts — the agent reads the reason and retries.
 */
#define _POSIX_C_SOURCE 200809L

#include "ops.h"
#include "build.h"

#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <dlfcn.h>
#include <sys/stat.h>
#include <ctype.h>
#include <math.h>

/* ---------------- result helpers ---------------- */

static OpResult op_ok(void)        { OpResult r = {1, OP_OK, 0, {0}}; return r; }
static OpResult op_okv(long value) { OpResult r = {1, OP_OK, value, {0}}; return r; }

static OpResult op_err(OpReason reason, const char *fmt, ...) {
    OpResult r = {0, reason, 0, {0}};
    va_list ap; va_start(ap, fmt);
    vsnprintf(r.detail, sizeof r.detail, fmt, ap);
    va_end(ap);
    return r;
}

/* public constructors (used by separately-compiled meta-op-authored ops) */
OpResult op_result_ok(long value) { return op_okv(value); }
OpResult op_result_error(OpReason reason, const char *detail) {
    OpResult r = {0, reason, 0, {0}};
    snprintf(r.detail, sizeof r.detail, "%s", detail ? detail : "");
    return r;
}

const char *op_reason_name(OpReason r) {
    switch (r) {
    case OP_OK:               return "ok";
    case OP_ERR_NO_SUCH_OP:   return "no_such_op";
    case OP_ERR_BAD_ARG:      return "bad_arg";
    case OP_ERR_MISSING_ARG:  return "missing_arg";
    case OP_ERR_NO_SUCH_FILE: return "no_such_file";
    case OP_ERR_FILE_ABSENT:  return "file_absent";
    case OP_ERR_NO_SUCH_BRANCH:return "no_such_branch";
    case OP_ERR_BAD_OFFSET:   return "bad_offset";
    case OP_ERR_BAD_RANGE:    return "bad_range";
    case OP_ERR_NOTHING_TO_DO: return "nothing_to_do";
    case OP_ERR_BUILD_FAILED: return "build_failed";
    case OP_ERR_RUN_FAILED:   return "run_failed";
    case OP_ERR_INTERNAL:     return "internal";
    }
    return "unknown";
}

/* ---------------- argument bag ---------------- */

#define OP_ARGS_MAX 24

typedef struct {
    char  key[40];
    int   is_int;
    long  ival;
    char *sval;      /* owned */
} OpArgEntry;

struct OpArgs {
    OpArgEntry e[OP_ARGS_MAX];
    size_t     n;
};

OpArgs *op_args_new(void) {
    OpArgs *a = calloc(1, sizeof *a);
    return a;
}

void op_args_free(OpArgs *a) {
    if (!a) return;
    for (size_t i = 0; i < a->n; i++) free(a->e[i].sval);
    free(a);
}

static char *op_dup(const char *s) {   /* strdup without the POSIX feature-macro dance */
    if (!s) return NULL;
    size_t n = strlen(s) + 1;
    char *p = malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

static OpArgEntry *slot(OpArgs *a, const char *key) {
    for (size_t i = 0; i < a->n; i++)
        if (strcmp(a->e[i].key, key) == 0) return &a->e[i];
    if (a->n >= OP_ARGS_MAX) return NULL;
    OpArgEntry *s = &a->e[a->n++];
    snprintf(s->key, sizeof s->key, "%s", key);
    s->is_int = 0; s->ival = 0; s->sval = NULL;
    return s;
}

void op_args_set_str(OpArgs *a, const char *key, const char *val) {
    if (!a) return;
    OpArgEntry *s = slot(a, key);
    if (!s) return;
    free(s->sval);
    s->is_int = 0;
    s->sval = op_dup(val);
}

void op_args_set_int(OpArgs *a, const char *key, long val) {
    if (!a) return;
    OpArgEntry *s = slot(a, key);
    if (!s) return;
    free(s->sval); s->sval = NULL;
    s->is_int = 1; s->ival = val;
}

static const OpArgEntry *find(const OpArgs *a, const char *key) {
    if (!a) return NULL;
    for (size_t i = 0; i < a->n; i++)
        if (strcmp(a->e[i].key, key) == 0) return &a->e[i];
    return NULL;
}

const char *op_arg_str(const OpArgs *a, const char *key, const char *dflt) {
    const OpArgEntry *s = find(a, key);
    return (s && !s->is_int && s->sval) ? s->sval : dflt;
}

long op_arg_int(const OpArgs *a, const char *key, long dflt, int *found) {
    const OpArgEntry *s = find(a, key);
    if (s && s->is_int) { if (found) *found = 1; return s->ival; }
    if (found) *found = 0;
    return dflt;
}

int op_arg_has(const OpArgs *a, const char *key) { return find(a, key) != NULL; }

/* ---------------- shared validation ---------------- */

/* Resolve the optional `branch` arg. trunk -> *branch = -1.
 * On an invalid branch, fills *res and returns 0. */
static int resolve_branch(IdeCore *c, const OpArgs *a, long *branch, OpResult *res) {
    int found;
    long b = op_arg_int(a, "branch", -1, &found);
    if (!found) { *branch = -1; return 1; }
    if (b < 0 || !ide_branch_valid(c, (BranchId)b)) {
        *res = op_err(OP_ERR_NO_SUCH_BRANCH, "no branch %ld", b);
        return 0;
    }
    *branch = b;
    return 1;
}

/* A view onto trunk (branch < 0) or a branch. */
static IdeView view_of(IdeCore *c, long branch) {
    return (branch >= 0) ? ide_branch_view(c, (BranchId)branch) : ide_trunk_view(c);
}

/* Validate a FileId on trunk/branch; sets *len for present files.
 * Returns OP_OK, or a reason (NO_SUCH_FILE / FILE_ABSENT). */
static OpReason check_file(IdeCore *c, long branch, long file, uint32_t *len) {
    if (file < 0) return OP_ERR_NO_SUCH_FILE;
    IdeView v = view_of(c, branch);
    if ((size_t)file >= ide_view_file_count(v)) return OP_ERR_NO_SUCH_FILE;
    if (!ide_view_file_present(v, (FileId)file)) return OP_ERR_FILE_ABSENT;
    if (len) ide_view_file_text(v, (FileId)file, len);
    return OP_OK;
}

/* ---------------- ops ---------------- */

static OpResult op_create_file(IdeCore *c, const OpArgs *a, OpContext *ctx) {
    (void)ctx;
    const char *path = op_arg_str(a, "path", NULL);
    if (!path) return op_err(OP_ERR_MISSING_ARG, "path required");
    long br; OpResult e; if (!resolve_branch(c, a, &br, &e)) return e;
    FileId f = (br >= 0) ? ide_b_create_file(c, (BranchId)br, path)
                         : ide_create_file(c, path);
    return op_okv((long)f);
}

static OpResult op_delete_file(IdeCore *c, const OpArgs *a, OpContext *ctx) {
    (void)ctx;
    int found; long file = op_arg_int(a, "file", -1, &found);
    if (!found) return op_err(OP_ERR_MISSING_ARG, "file required");
    long br; OpResult e; if (!resolve_branch(c, a, &br, &e)) return e;
    OpReason fr = check_file(c, br, file, NULL);
    if (fr != OP_OK) return op_err(fr, "file %ld", file);
    if (br >= 0) ide_b_delete_file(c, (BranchId)br, (FileId)file);
    else         ide_delete_file(c, (FileId)file);
    return op_ok();
}

static OpResult op_rename_file(IdeCore *c, const OpArgs *a, OpContext *ctx) {
    (void)ctx;
    int found; long file = op_arg_int(a, "file", -1, &found);
    if (!found) return op_err(OP_ERR_MISSING_ARG, "file required");
    const char *path = op_arg_str(a, "path", NULL);
    if (!path) return op_err(OP_ERR_MISSING_ARG, "path required");
    long br; OpResult e; if (!resolve_branch(c, a, &br, &e)) return e;
    OpReason fr = check_file(c, br, file, NULL);
    if (fr != OP_OK) return op_err(fr, "file %ld", file);
    if (br >= 0) ide_b_rename_file(c, (BranchId)br, (FileId)file, path);
    else         ide_rename_file(c, (FileId)file, path);
    return op_ok();
}

static OpResult op_insert_text(IdeCore *c, const OpArgs *a, OpContext *ctx) {
    (void)ctx;
    int found;
    long file = op_arg_int(a, "file", -1, &found);
    if (!found) return op_err(OP_ERR_MISSING_ARG, "file required");
    long offset = op_arg_int(a, "offset", -1, &found);
    if (!found) return op_err(OP_ERR_MISSING_ARG, "offset required");
    const char *text = op_arg_str(a, "text", NULL);
    if (!text) return op_err(OP_ERR_MISSING_ARG, "text required");
    long br; OpResult e; if (!resolve_branch(c, a, &br, &e)) return e;
    uint32_t len = 0;
    OpReason fr = check_file(c, br, file, &len);
    if (fr != OP_OK) return op_err(fr, "file %ld", file);
    if (offset < 0 || (uint32_t)offset > len)
        return op_err(OP_ERR_BAD_OFFSET, "offset %ld out of 0..%u", offset, len);
    if (br >= 0) ide_b_insert(c, (BranchId)br, (FileId)file, (uint32_t)offset, text);
    else         ide_insert(c, (FileId)file, (uint32_t)offset, text);
    return op_ok();
}

static OpResult op_delete_text(IdeCore *c, const OpArgs *a, OpContext *ctx) {
    (void)ctx;
    int found;
    long file = op_arg_int(a, "file", -1, &found);
    if (!found) return op_err(OP_ERR_MISSING_ARG, "file required");
    long offset = op_arg_int(a, "offset", -1, &found);
    if (!found) return op_err(OP_ERR_MISSING_ARG, "offset required");
    long n = op_arg_int(a, "len", -1, &found);
    if (!found) return op_err(OP_ERR_MISSING_ARG, "len required");
    long br; OpResult e; if (!resolve_branch(c, a, &br, &e)) return e;
    uint32_t len = 0;
    OpReason fr = check_file(c, br, file, &len);
    if (fr != OP_OK) return op_err(fr, "file %ld", file);
    if (offset < 0 || (uint32_t)offset > len)
        return op_err(OP_ERR_BAD_OFFSET, "offset %ld out of 0..%u", offset, len);
    if (n < 0 || (uint32_t)offset + (uint32_t)n > len)
        return op_err(OP_ERR_BAD_RANGE, "range %ld..%ld out of 0..%u", offset, offset + n, len);
    if (br >= 0) ide_b_delete(c, (BranchId)br, (FileId)file, (uint32_t)offset, (uint32_t)n);
    else         ide_delete(c, (FileId)file, (uint32_t)offset, (uint32_t)n);
    return op_ok();
}

static OpResult op_undo(IdeCore *c, const OpArgs *a, OpContext *ctx) {
    (void)ctx;
    long br; OpResult e; if (!resolve_branch(c, a, &br, &e)) return e;
    if (br >= 0) ide_b_undo(c, (BranchId)br); else ide_undo(c);
    return op_ok();
}

static OpResult op_redo(IdeCore *c, const OpArgs *a, OpContext *ctx) {
    (void)ctx;
    long br; OpResult e; if (!resolve_branch(c, a, &br, &e)) return e;
    if (br >= 0) ide_b_redo(c, (BranchId)br); else ide_redo(c);
    return op_ok();
}

static OpResult op_fork(IdeCore *c, const OpArgs *a, OpContext *ctx) {
    (void)a; (void)ctx;
    BranchId b = ide_fork(c);
    return op_okv((long)b);
}

/* merge/discard take a *required* branch (not the optional trunk-or-branch arg). */
static int require_branch(IdeCore *c, const OpArgs *a, long *out, OpResult *res) {
    int found; long b = op_arg_int(a, "branch", -1, &found);
    if (!found) { *res = op_err(OP_ERR_MISSING_ARG, "branch required"); return 0; }
    if (b < 0 || !ide_branch_valid(c, (BranchId)b)) {
        *res = op_err(OP_ERR_NO_SUCH_BRANCH, "no branch %ld", b); return 0;
    }
    *out = b; return 1;
}

static OpResult op_merge(IdeCore *c, const OpArgs *a, OpContext *ctx) {
    (void)ctx;
    long b; OpResult e; if (!require_branch(c, a, &b, &e)) return e;
    ide_branch_merge(c, (BranchId)b);
    return op_ok();
}

static OpResult op_discard(IdeCore *c, const OpArgs *a, OpContext *ctx) {
    (void)ctx;
    long b; OpResult e; if (!require_branch(c, a, &b, &e)) return e;
    ide_branch_discard(c, (BranchId)b);
    return op_ok();
}

static OpResult op_build(IdeCore *c, const OpArgs *a, OpContext *ctx) {
    long br; OpResult e; if (!resolve_branch(c, a, &br, &e)) return e;
    const char *wd = ctx ? ctx->workdir : NULL;
    if (!wd) return op_err(OP_ERR_BAD_ARG, "no workdir (set OpContext.workdir)");
    BuildResult r = build_project_view(view_of(c, br), wd);
    if (ctx && ctx->out) {
        for (size_t i = 0; i < r.diag_count; i++) {
            const char *sev = r.diags[i].severity == DIAG_ERROR   ? "error"
                            : r.diags[i].severity == DIAG_WARNING ? "warning" : "note";
            fprintf(ctx->out, "%s:%d:%d: %s: %s\n",
                    r.diags[i].file ? r.diags[i].file : "?",
                    r.diags[i].line, r.diags[i].col, sev,
                    r.diags[i].message ? r.diags[i].message : "");
        }
    }
    OpResult res;
    if (r.ok) {
        res = op_okv(r.warning_count);
        snprintf(res.detail, sizeof res.detail, "built %s (%d warning(s))",
                 r.product ? r.product : "(no product)", r.warning_count);
    } else {
        res = op_err(OP_ERR_BUILD_FAILED, "%d error(s), %d warning(s)",
                     r.error_count, r.warning_count);
        res.value = r.error_count;
    }
    build_result_free(&r);
    return res;
}

static OpResult op_list_files(IdeCore *c, const OpArgs *a, OpContext *ctx) {
    long br; OpResult e; if (!resolve_branch(c, a, &br, &e)) return e;
    IdeView v = view_of(c, br);
    size_t n = ide_view_file_count(v), shown = 0;
    for (FileId f = 0; f < n; f++) {
        if (!ide_view_file_present(v, f)) continue;
        uint32_t len = 0; ide_view_file_text(v, f, &len);
        if (ctx && ctx->out)
            fprintf(ctx->out, "%u\t%s\t%u bytes\n", f, ide_view_file_path(v, f), len);
        shown++;
    }
    return op_okv((long)shown);
}

static OpResult op_inspect(IdeCore *c, const OpArgs *a, OpContext *ctx) {
    int found; long file = op_arg_int(a, "file", -1, &found);
    if (!found) return op_err(OP_ERR_MISSING_ARG, "file required");
    long br; OpResult e; if (!resolve_branch(c, a, &br, &e)) return e;
    uint32_t len = 0;
    OpReason fr = check_file(c, br, file, &len);
    if (fr != OP_OK) return op_err(fr, "file %ld", file);
    if (ctx && ctx->out) {
        const char *txt = ide_view_file_text(view_of(c, br), (FileId)file, &len);
        fwrite(txt, 1, len, ctx->out);
    }
    return op_okv((long)len);
}

static OpResult op_schema_op(IdeCore *c, const OpArgs *a, OpContext *ctx) {
    (void)c; (void)a;
    if (ctx && ctx->out) forge_schema_text(ctx->out);   /* the reader-agnostic projection */
    return op_ok();
}

static OpResult op_archetypes_op(IdeCore *c, const OpArgs *a, OpContext *ctx) {
    (void)c; (void)a;
    if (ctx && ctx->out) forge_archetypes(ctx->out);
    return op_ok();
}

static OpResult op_find_op(IdeCore *c, const OpArgs *a, OpContext *ctx) {
    (void)c;
    const char *intent = op_arg_str(a, "intent", NULL);
    if (!intent) return op_err(OP_ERR_MISSING_ARG, "intent required");
    int found; long k = op_arg_int(a, "k", 5, &found);
    if (k < 1) k = 5;
    if (k > 20) k = 20;
    OpHit hits[20];
    size_t n = forge_find_op(intent, hits, (size_t)k);
    if (ctx && ctx->out)
        for (size_t i = 0; i < n; i++) {
            const OpDef *d = forge_op_find(hits[i].name);
            fprintf(ctx->out, "%.3f  %-14s (%s)\n\t%s\n", hits[i].score, hits[i].name,
                    d ? d->arg_sig : "", d ? d->doc : "");
        }
    return op_okv((long)n);
}

static OpResult op_grammar(IdeCore *c, const OpArgs *a, OpContext *ctx) {
    (void)c;
    const char *intent = op_arg_str(a, "intent", NULL);
    if (!intent) return op_err(OP_ERR_MISSING_ARG, "intent required");
    int found; long k = op_arg_int(a, "k", 5, &found);
    if (k < 1) k = 5;
    if (k > 20) k = 20;
    if (!ctx || !ctx->out) return op_err(OP_ERR_BAD_ARG, "no output sink for the grammar");
    size_t n = forge_grammar_for_intent(intent, (size_t)k, ctx->out);
    return op_okv((long)n);
}

static OpResult op_jsonschema(IdeCore *c, const OpArgs *a, OpContext *ctx) {
    (void)c;
    const char *intent = op_arg_str(a, "intent", NULL);
    if (!intent) return op_err(OP_ERR_MISSING_ARG, "intent required");
    int found; long k = op_arg_int(a, "k", 5, &found);
    if (k < 1) k = 5;
    if (k > 20) k = 20;
    if (!ctx || !ctx->out) return op_err(OP_ERR_BAD_ARG, "no output sink for the schema");
    size_t n = forge_jsonschema_for_intent(intent, (size_t)k, ctx->out);
    return op_okv((long)n);
}

static OpResult op_catalog(IdeCore *c, const OpArgs *a, OpContext *ctx) {
    (void)c; (void)a;
    if (ctx && ctx->out) forge_catalog(ctx->out);
    return op_ok();
}

/* ===================== meta-ops: forge grows its own op set ===================== *
 * propose -> test (compile) -> accept (dlopen + register live) -> reject.
 * The accepted op is a function in a dlopen'd dylib, dispatched exactly like a
 * built-in. This is the cycle sh3d does via system("make") + relaunch; forge's
 * in-process compile + dlopen makes it live in one session. */

static ForgeToolchain g_meta_tc;
static int  g_meta_ready = 0;
static char g_meta_scratch[512] = ".";
static char g_meta_include[512] = ".";

void forge_meta_configure(ForgeToolchain tc, const char *scratch_dir, const char *include_dir) {
    g_meta_tc = tc;
    snprintf(g_meta_scratch, sizeof g_meta_scratch, "%s", scratch_dir ? scratch_dir : ".");
    snprintf(g_meta_include, sizeof g_meta_include, "%s", include_dir ? include_dir : ".");
    mkdir(g_meta_scratch, 0777);   /* ok if present */
    g_meta_ready = 1;
}

/* session-scoped proposals (authored, not yet accepted) */
typedef struct {
    char *name, *sig, *doc;
    int   mutates;        /* 1 = changes state (gets a transaction + an inverse check) */
    char *probe_seed;     /* fixture file contents the requirements gate runs against */
    char *probe_args;     /* extra probe args "k=v,k=v" the gate passes the op */
} Proposal;
static Proposal *g_prop; static size_t g_prop_n, g_prop_cap;

/* live, dlopen'd ops registered after accept */
typedef struct { OpDef def; void *handle; char *name, *sig, *doc; int mutates; } DynOp;
static DynOp *g_dyn; static size_t g_dyn_n, g_dyn_cap;

static int valid_op_name(const char *n) {
    if (!n || !*n) return 0;
    if (n[0] >= '0' && n[0] <= '9') return 0;
    for (const char *p = n; *p; p++) {
        char c = *p;
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) return 0;
    }
    return 1;
}

static Proposal *prop_find(const char *name) {
    for (size_t i = 0; i < g_prop_n; i++)
        if (strcmp(g_prop[i].name, name) == 0) return &g_prop[i];
    return NULL;
}
static void prop_remove(const char *name) {
    for (size_t i = 0; i < g_prop_n; i++)
        if (strcmp(g_prop[i].name, name) == 0) {
            free(g_prop[i].name); free(g_prop[i].sig); free(g_prop[i].doc);
            free(g_prop[i].probe_seed); free(g_prop[i].probe_args);
            g_prop[i] = g_prop[--g_prop_n];
            return;
        }
}
static void meta_paths(const char *name, char *src, size_t sc, char *lib, size_t lc) {
    snprintf(src, sc, "%s/forge_op_%s.c",  g_meta_scratch, name);
    snprintf(lib, lc, "%s/forge_op_%s.so", g_meta_scratch, name);
}

/* ---- archetypes: op skeletons that encode the forward-pass discipline. The
 * author fills only a small <<BODY>> blank and gets arg validation, atomic
 * state change, and invertibility for free (the design rule: mutate ONLY through
 * the ide_* primitives, and one undo reverses the whole op). ---- */
typedef struct {
    const char *name;
    int         mutates;
    const char *fills;   /* what the author must set in <<BODY>> */
    const char *body;    /* skeleton with one <<BODY>> slot */
} Archetype;

static const Archetype ARCH[] = {
    { "compute", 0,
      "set `long result` from args (op_arg_int/op_arg_str). PURE — must not mutate.",
      "    long result = 0;\n"
      "    { <<BODY>> }\n"
      "    return op_result_ok(result);\n" },
    { "transform_file", 1,
      "set `char *out` (malloc'd, NUL-terminated) from `char *in` (the file text); "
      "e.g. `out = your_pure_fn(in);`",
      "    int ff; long file = op_arg_int(args, \"file\", -1, &ff);\n"
      "    if (!ff) return op_result_error(OP_ERR_MISSING_ARG, \"file\");\n"
      "    uint32_t len = 0; const char *src = ide_file_text(core, (FileId)file, &len);\n"
      "    char *in = (char *)malloc((size_t)len + 1);\n"
      "    if (!in) return op_result_error(OP_ERR_INTERNAL, \"oom\");\n"
      "    memcpy(in, src, len); in[len] = 0;\n"
      "    char *out = 0;\n"
      "    { <<BODY>> }\n"
      "    if (!out) { free(in); return op_result_error(OP_ERR_BAD_ARG, \"no output\"); }\n"
      "    ide_delete(core, (FileId)file, 0, len);\n"
      "    ide_insert(core, (FileId)file, 0, out);\n"
      "    free(in); free(out);\n"
      "    return op_result_ok(0);\n" },
    { "insert_at", 1,
      "set `char *ins` (malloc'd, NUL-terminated) — the text to insert at `offset`.",
      "    int ff; long file = op_arg_int(args, \"file\", -1, &ff);\n"
      "    if (!ff) return op_result_error(OP_ERR_MISSING_ARG, \"file\");\n"
      "    int fo; long off = op_arg_int(args, \"offset\", -1, &fo);\n"
      "    if (!fo) return op_result_error(OP_ERR_MISSING_ARG, \"offset\");\n"
      "    uint32_t flen = 0; ide_file_text(core, (FileId)file, &flen);\n"
      "    if (off < 0 || (uint32_t)off > flen) return op_result_error(OP_ERR_BAD_OFFSET, \"offset\");\n"
      "    char *ins = 0;\n"
      "    { <<BODY>> }\n"
      "    if (ins) { ide_insert(core, (FileId)file, (uint32_t)off, ins); free(ins); }\n"
      "    return op_result_ok(0);\n" },
};
#define ARCH_N (sizeof ARCH / sizeof ARCH[0])

static const Archetype *archetype_find(const char *name) {
    if (!name) return NULL;
    for (size_t i = 0; i < ARCH_N; i++) if (strcmp(ARCH[i].name, name) == 0) return &ARCH[i];
    return NULL;
}

void forge_archetypes(FILE *out) {
    if (!out) return;
    fprintf(out, "# op archetypes — %zu (pick one, fill <<BODY>>; the skeleton governs the rest)\n", ARCH_N);
    for (size_t i = 0; i < ARCH_N; i++)
        fprintf(out, "%-15s %-7s  fill: %s\n",
                ARCH[i].name, ARCH[i].mutates ? "mutate" : "query", ARCH[i].fills);
}

/* splice the author's body into the archetype's <<BODY>> slot */
static char *archetype_expand(const char *tmpl, const char *body) {
    const char *slot = strstr(tmpl, "<<BODY>>");
    if (!slot) return op_dup(tmpl);
    size_t pre = (size_t)(slot - tmpl), blen = strlen(body);
    char *out = (char *)malloc(strlen(tmpl) - 8 + blen + 1);   /* 8 = strlen("<<BODY>>") */
    memcpy(out, tmpl, pre);
    memcpy(out + pre, body, blen);
    strcpy(out + pre + blen, slot + 8);
    return out;
}

static OpResult m_propose(IdeCore *c, const OpArgs *a, OpContext *ctx) {
    (void)c; (void)ctx;
    const char *name = op_arg_str(a, "name", NULL);
    const char *sig  = op_arg_str(a, "arg_sig", "");
    const char *doc  = op_arg_str(a, "doc", "(no doc)");
    const char *body = op_arg_str(a, "body", NULL);
    if (!name) return op_err(OP_ERR_MISSING_ARG, "name required");
    if (!body) return op_err(OP_ERR_MISSING_ARG, "body required");
    if (!valid_op_name(name)) return op_err(OP_ERR_BAD_ARG, "illegal op name '%s'", name);
    if (forge_op_find(name))  return op_err(OP_ERR_BAD_ARG, "op '%s' already exists", name);
    if (!g_meta_ready) return op_err(OP_ERR_INTERNAL, "meta not configured");

    const char *arch_name = op_arg_str(a, "archetype", NULL);
    const Archetype *arch = NULL;
    if (arch_name) {
        arch = archetype_find(arch_name);
        if (!arch) return op_err(OP_ERR_BAD_ARG, "no archetype '%s'", arch_name);
    }
    int found;
    int mutates = (int)op_arg_int(a, "mutates", arch ? arch->mutates : 1, &found);

    /* archetype skeleton with <<BODY>> filled, or freeform body */
    char *gen = arch ? archetype_expand(arch->body, body) : op_dup(body);

    char src[600], lib[600];
    meta_paths(name, src, sizeof src, lib, sizeof lib);
    FILE *f = fopen(src, "w");
    if (!f) { free(gen); return op_err(OP_ERR_INTERNAL, "cannot write %s", src); }
    fprintf(f,
        "#include \"ops.h\"\n"
        "#include <stdlib.h>\n#include <string.h>\n#include <ctype.h>\n"
        "OpResult forge_dynop(IdeCore *core, const OpArgs *args, OpContext *ctx) {\n"
        "    (void)core; (void)args; (void)ctx;\n"
        "%s\n"
        "}\n", gen);
    fclose(f);
    free(gen);

    prop_remove(name);   /* re-propose replaces */
    if (g_prop_n == g_prop_cap) {
        g_prop_cap = g_prop_cap ? g_prop_cap * 2 : 8;
        g_prop = realloc(g_prop, g_prop_cap * sizeof *g_prop);
    }
    Proposal *pr = &g_prop[g_prop_n++];
    pr->name = op_dup(name);
    pr->sig  = op_dup(sig);
    pr->doc  = op_dup(doc);
    pr->mutates = mutates;
    pr->probe_seed = op_dup(op_arg_str(a, "probe_seed", ""));
    pr->probe_args = op_dup(op_arg_str(a, "probe_args", ""));
    return op_ok();
}

static OpResult meta_compile(const char *name, OpContext *ctx) {
    char src[600], lib[600];
    meta_paths(name, src, sizeof src, lib, sizeof lib);
    const char *argv[11];
    int i = 0;
    argv[i++] = "cc"; argv[i++] = "-std=c11"; argv[i++] = "-fPIC"; argv[i++] = "-shared";
    argv[i++] = "-I"; argv[i++] = g_meta_include;
    argv[i++] = src; argv[i++] = "-o"; argv[i++] = lib; argv[i] = NULL;
    int code = 0;
    char *o = g_meta_tc.exec(argv, &code, g_meta_tc.ctx);
    if (ctx && ctx->out && o && *o) fputs(o, ctx->out);
    free(o);
    return code == 0 ? op_ok() : op_err(OP_ERR_BUILD_FAILED, "compile failed (rc=%d)", code);
}

/* parse "k=v,k=v" into args (int if the value is all digits, else a string) */
static void parse_probe_args(OpArgs *a, const char *spec) {
    if (!spec || !*spec) return;
    char *s = op_dup(spec), *p = s;
    while (p && *p) {
        char *comma = strchr(p, ',');
        if (comma) *comma = 0;
        char *eq = strchr(p, '=');
        if (eq) {
            *eq = 0;
            const char *key = p, *val = eq + 1;
            int isint = (*val != 0);
            for (const char *v = (*val == '-' ? val + 1 : val); *v; v++)
                if (*v < '0' || *v > '9') { isint = 0; break; }
            if (isint) op_args_set_int(a, key, atol(val));
            else       op_args_set_str(a, key, val);
        }
        p = comma ? comma + 1 : NULL;
    }
    free(s);
}

/* The REQUIREMENTS GATE: run the op on a fixture and verify the contract —
 * it succeeds on its probe, fold holds, a query doesn't mutate, and a mutating
 * op is invertible (undo restores the prior state byte-for-byte). */
static OpResult meta_gate(OpFn fn, const Proposal *pr) {
    IdeCore *fc = ide_create();
    FileId pf = ide_create_file(fc, "probe.txt");
    if (pr->probe_seed && *pr->probe_seed) ide_insert(fc, pf, 0, pr->probe_seed);

    uint32_t plen = 0; const char *ptext = ide_file_text(fc, pf, &plen);
    char *pre = (char *)malloc((size_t)plen + 1);
    memcpy(pre, ptext, plen); pre[plen] = 0;

    OpArgs *a = op_args_new();
    op_args_set_int(a, "file", (long)pf);
    parse_probe_args(a, pr->probe_args);
    OpContext ctx = {0};

    if (pr->mutates) ide_group_begin(fc);
    OpResult r = fn(fc, a, &ctx);
    if (pr->mutates) ide_group_end(fc);

    OpResult gate = op_ok();
    if (!r.ok) {
        gate = op_err(OP_ERR_BUILD_FAILED, "gate: op errored on its probe (%s)", r.detail);
    } else if (!ide_verify_fold(fc)) {
        gate = op_err(OP_ERR_BUILD_FAILED, "gate: fold broken after the op");
    } else {
        uint32_t qlen = 0; const char *qt = ide_file_text(fc, pf, &qlen);
        int changed = (qlen != plen) || (plen && memcmp(qt, pre, plen) != 0);
        if (!pr->mutates && changed)
            gate = op_err(OP_ERR_BUILD_FAILED, "gate: a 'query' op mutated state");
        else if (pr->mutates) {
            ide_undo(fc);
            uint32_t ulen = 0; const char *ut = ide_file_text(fc, pf, &ulen);
            int restored = (ulen == plen) && (!plen || memcmp(ut, pre, plen) == 0);
            if (!restored)
                gate = op_err(OP_ERR_BUILD_FAILED, "gate: op is not invertible (undo did not restore state)");
        }
    }
    free(pre); op_args_free(a); ide_destroy(fc);
    return gate;
}

/* compile + dlopen + resolve the candidate's entry; on success returns handle+fn. */
static OpResult meta_build(const char *name, OpContext *ctx, void **out_h, OpFn *out_fn) {
    OpResult cr = meta_compile(name, ctx);
    if (!cr.ok) return cr;
    char src[600], lib[600];
    meta_paths(name, src, sizeof src, lib, sizeof lib);
    void *h = dlopen(lib, RTLD_NOW | RTLD_LOCAL);
    if (!h) { const char *e = dlerror(); return op_err(OP_ERR_INTERNAL, "dlopen: %s", e ? e : "?"); }
    void *sym = dlsym(h, "forge_dynop");
    if (!sym) { dlclose(h); return op_err(OP_ERR_INTERNAL, "entry 'forge_dynop' missing"); }
    *out_h = h;
    *(void **)out_fn = sym;
    return op_ok();
}

static OpResult m_test(IdeCore *c, const OpArgs *a, OpContext *ctx) {
    (void)c;
    const char *name = op_arg_str(a, "name", NULL);
    if (!name) return op_err(OP_ERR_MISSING_ARG, "name required");
    Proposal *pr = prop_find(name);
    if (!pr) return op_err(OP_ERR_NO_SUCH_FILE, "no proposal '%s'", name);
    void *h = NULL; OpFn fn = NULL;
    OpResult br = meta_build(name, ctx, &h, &fn);   /* compile + load */
    if (!br.ok) return br;
    OpResult g = meta_gate(fn, pr);                 /* + the requirements gate */
    dlclose(h);
    return g;
}

static OpResult m_accept(IdeCore *c, const OpArgs *a, OpContext *ctx) {
    (void)c;
    const char *name = op_arg_str(a, "name", NULL);
    if (!name) return op_err(OP_ERR_MISSING_ARG, "name required");
    Proposal *pr = prop_find(name);
    if (!pr) return op_err(OP_ERR_NO_SUCH_FILE, "no proposal '%s'", name);

    void *h = NULL; OpFn fn = NULL;
    OpResult br = meta_build(name, ctx, &h, &fn);
    if (!br.ok) return br;
    OpResult g = meta_gate(fn, pr);       /* ENFORCED: a failing op never goes live */
    if (!g.ok) { dlclose(h); return g; }

    if (g_dyn_n == g_dyn_cap) {
        g_dyn_cap = g_dyn_cap ? g_dyn_cap * 2 : 8;
        g_dyn = realloc(g_dyn, g_dyn_cap * sizeof *g_dyn);
    }
    DynOp *d = &g_dyn[g_dyn_n++];
    d->handle = h;
    d->name = op_dup(pr->name);
    d->sig  = op_dup(pr->sig);
    d->doc  = op_dup(pr->doc);
    d->mutates = pr->mutates;
    d->def.name = d->name; d->def.arg_sig = d->sig; d->def.doc = d->doc;
    d->def.mutates = pr->mutates; d->def.fn = fn;
    prop_remove(name);
    return op_ok();
}

static OpResult m_reject(IdeCore *c, const OpArgs *a, OpContext *ctx) {
    (void)c; (void)ctx;
    const char *name = op_arg_str(a, "name", NULL);
    if (!name) return op_err(OP_ERR_MISSING_ARG, "name required");
    if (!prop_find(name)) return op_ok();   /* idempotent */
    char src[600], lib[600];
    meta_paths(name, src, sizeof src, lib, sizeof lib);
    remove(src); remove(lib);
    prop_remove(name);
    return op_ok();
}

/* ---------------- the registry (dispatcher + schema derive from this) ---------------- */

static const OpDef TABLE[] = {
    {"create_file", "path:str, [branch:int]",
     "Create a file; returns its FileId.", 1, op_create_file},
    {"delete_file", "file:int, [branch:int]",
     "Delete a file (reversible; bytes are kept).", 1, op_delete_file},
    {"rename_file", "file:int, path:str, [branch:int]",
     "Rename a file.", 1, op_rename_file},
    {"insert_text", "file:int, offset:int, text:str, [branch:int]",
     "Insert bytes at offset.", 1, op_insert_text},
    {"delete_text", "file:int, offset:int, len:int, [branch:int]",
     "Delete len bytes at offset (kept for undo).", 1, op_delete_text},
    {"undo", "[branch:int]", "Undo the last edit.", 1, op_undo},
    {"redo", "[branch:int]", "Redo the last undone edit.", 1, op_redo},
    {"fork", "", "Open a speculative sandbox branch; returns its BranchId.", 1, op_fork},
    {"merge", "branch:int", "Adopt a branch's edits onto trunk, then drop it.", 1, op_merge},
    {"discard", "branch:int", "Drop a branch; trunk stays byte-identical.", 1, op_discard},
    {"build", "[branch:int]",
     "Compile trunk or a branch; returns error count (0 = green).", 0, op_build},
    {"list_files", "[branch:int]",
     "List present files (FileId, path, size); returns count.", 0, op_list_files},
    {"inspect", "file:int, [branch:int]",
     "Print a file's contents; returns byte length.", 0, op_inspect},
    {"schema", "", "Print the plain-text, reader-agnostic op schema.", 0, op_schema_op},
    {"catalog", "", "Print the human-readable op catalog (Markdown).", 0, op_catalog},
    {"archetypes", "", "List op archetypes (skeletons for authoring new ops).", 0, op_archetypes_op},
    {"find_op", "intent:str, [k:int]",
     "Search the op set by intent; ranks the best-matching ops (retrieval, not enumeration).", 0, op_find_op},
    {"grammar", "intent:str, [k:int]",
     "Emit a GBNF grammar constraining a small model to a valid call to one of the top-k ops.", 0, op_grammar},
    {"jsonschema", "intent:str, [k:int]",
     "Emit a JSON Schema constraining a structured-output model (e.g. Claude) to a valid op call.", 0, op_jsonschema},
    /* meta-ops: forge authors/compiles/loads its own ops at runtime */
    {"propose_op", "name:str, arg_sig:str, doc:str, body:str, [archetype:str], [mutates:int], [probe_seed:str], [probe_args:str]",
     "Author a new op: fill an archetype's <<BODY>> (or freeform body); writes its source.", 0, m_propose},
    {"test_op", "name:str",
     "Compile a proposed op AND run the requirements gate (fold + invertibility on a probe).", 0, m_test},
    {"accept_op", "name:str",
     "Gate, then dlopen + register the op live (no relaunch). A failing op never goes live.", 0, m_accept},
    {"reject_op", "name:str", "Drop a proposed op.", 0, m_reject},
};

#define TABLE_N (sizeof TABLE / sizeof TABLE[0])

const OpDef *forge_op_table(size_t *count) {
    if (count) *count = TABLE_N;
    return TABLE;
}

const OpDef *forge_op_find(const char *name) {
    if (!name) return NULL;
    for (size_t i = 0; i < TABLE_N; i++)
        if (strcmp(TABLE[i].name, name) == 0) return &TABLE[i];
    for (size_t i = 0; i < g_dyn_n; i++)        /* dynamically-accepted ops */
        if (strcmp(g_dyn[i].def.name, name) == 0) return &g_dyn[i].def;
    return NULL;
}

OpResult forge_op_apply(IdeCore *core, const char *op_name,
                        const OpArgs *args, OpContext *ctx) {
    const OpDef *def = forge_op_find(op_name);
    if (!def) return op_err(OP_ERR_NO_SUCH_OP, "no op '%s'", op_name ? op_name : "(null)");
    /* A dynamic (meta-op-authored) op may compose several primitives; bracket it
     * in a trunk transaction so one undo reverses the whole op atomically.
     * Built-ins each emit a single event, so they need no wrapping. */
    int dyn = 0;
    for (size_t i = 0; i < g_dyn_n; i++) if (&g_dyn[i].def == def) { dyn = 1; break; }
    int wrap = dyn && core && def->mutates;   /* only mutating dynamic ops need a transaction */
    if (wrap) ide_group_begin(core);
    OpResult r = def->fn(core, args, ctx);
    if (wrap) ide_group_end(core);
    return r;
}

void forge_schema(FILE *out) {
    if (!out) return;
    fprintf(out, "# forge op schema — %zu ops\n", TABLE_N + g_dyn_n);
    for (size_t i = 0; i < TABLE_N; i++)
        fprintf(out, "%-12s %-8s (%s)\n\t%s\n",
                TABLE[i].name, TABLE[i].mutates ? "mutate" : "query",
                TABLE[i].arg_sig, TABLE[i].doc);
    for (size_t i = 0; i < g_dyn_n; i++)        /* self-authored ops the agent can now use */
        fprintf(out, "%-12s %-8s (%s)  [dynamic]\n\t%s\n",
                g_dyn[i].def.name, "mutate", g_dyn[i].def.arg_sig, g_dyn[i].def.doc);
}

/* ===================== find_op: BM25 retrieval over the op set ===================== *
 * Each op's "document" is its breadcrumb: name + arg_sig + doc. A small model can
 * search by intent instead of holding the whole kernel in context — so the op set
 * can grow without limit while selection cost stays flat (OAT's method, over our
 * own schema). Index is computed on the fly: the op set is tiny and changes live. */

static const OpDef *op_at(size_t i) {
    return i < TABLE_N ? &TABLE[i] : &g_dyn[i - TABLE_N].def;
}

static int is_stop(const char *t, size_t n) {
    static const char *stops[] = { "a","an","the","to","of","from","in","at","on","with",
        "and","or","for","my","this","that","is","it","be","as","by","into",0 };
    for (int i = 0; stops[i]; i++)
        if (strlen(stops[i]) == n && strncmp(stops[i], t, n) == 0) return 1;
    return 0;
}

/* tokenize `s` (lowercase alphanumeric runs, drop stopwords + len<2). If `term`,
 * return its occurrence count; else return the total token count (doc length). */
static size_t tok_count(const char *s, const char *term) {
    if (!s) return 0;
    size_t total = 0, hits = 0, tl = term ? strlen(term) : 0;
    for (const char *p = s; *p; ) {
        while (*p && !isalnum((unsigned char)*p)) p++;
        const char *start = p;
        while (*p && isalnum((unsigned char)*p)) p++;
        size_t len = (size_t)(p - start);
        if (len < 2 || is_stop(start, len)) continue;
        total++;
        if (term && len == tl) {
            size_t i = 0;
            for (; i < len; i++) if ((char)tolower((unsigned char)start[i]) != term[i]) break;
            if (i == len) hits++;
        }
    }
    return term ? hits : total;
}

/* an op's term frequency (or doc length when term==NULL) across its breadcrumb */
static size_t op_tf(const OpDef *d, const char *term) {
    return tok_count(d->name, term) + tok_count(d->arg_sig, term) + tok_count(d->doc, term);
}

/* lowercase the query into unique, filtered terms */
static int query_terms(const char *q, char terms[][32], int max) {
    int n = 0;
    for (const char *p = q; *p && n < max; ) {
        while (*p && !isalnum((unsigned char)*p)) p++;
        const char *s = p;
        while (*p && isalnum((unsigned char)*p)) p++;
        size_t len = (size_t)(p - s);
        if (len < 2 || len >= 32 || is_stop(s, len)) continue;
        char t[32];
        for (size_t i = 0; i < len; i++) t[i] = (char)tolower((unsigned char)s[i]);
        t[len] = 0;
        int dup = 0;
        for (int j = 0; j < n; j++) if (strcmp(terms[j], t) == 0) { dup = 1; break; }
        if (!dup) strcpy(terms[n++], t);
    }
    return n;
}

size_t forge_find_op(const char *intent, OpHit *out, size_t max) {
    if (!intent || !out || max == 0) return 0;
    size_t N = TABLE_N + g_dyn_n;
    if (N == 0) return 0;

    double *score = (double *)calloc(N, sizeof(double));
    double *dl    = (double *)malloc(N * sizeof(double));
    if (!score || !dl) { free(score); free(dl); return 0; }

    double total = 0;
    for (size_t i = 0; i < N; i++) { dl[i] = (double)op_tf(op_at(i), NULL); total += dl[i]; }
    double avgdl = total / (double)N;
    if (avgdl <= 0) avgdl = 1;

    const double k1 = 1.2, b = 0.75;
    char terms[32][32];
    int qn = query_terms(intent, terms, 32);
    for (int q = 0; q < qn; q++) {
        size_t df = 0;
        for (size_t i = 0; i < N; i++) if (op_tf(op_at(i), terms[q]) > 0) df++;
        if (df == 0) continue;
        double idf = log(((double)N - (double)df + 0.5) / ((double)df + 0.5) + 1.0);
        for (size_t i = 0; i < N; i++) {
            double tf = (double)op_tf(op_at(i), terms[q]);
            if (tf <= 0) continue;
            score[i] += idf * (tf * (k1 + 1.0)) / (tf + k1 * (1.0 - b + b * dl[i] / avgdl));
        }
    }

    size_t got = 0;
    for (size_t k = 0; k < max; k++) {
        int best = -1; double bs = 0;
        for (size_t i = 0; i < N; i++) if (score[i] > bs) { bs = score[i]; best = (int)i; }
        if (best < 0) break;
        out[got].name  = op_at((size_t)best)->name;
        out[got].score = bs;
        got++;
        score[best] = -1;   /* remove from contention */
    }
    free(score); free(dl);
    return got;
}

/* ===================== GBNF grammar from candidate ops ===================== *
 * After find_op narrows to the top-k ops, emit a GBNF grammar that accepts ONLY a
 * JSON call  { "op": "<one of the k>", "args": { ...typed per that op... } }.
 * A grammar-constrained small model then *cannot* hallucinate an op or malform a
 * call — and the grammar is tiny (just the relevant ops), so generation is fast.
 * Retrieval + this grammar is the reliable, cheap tool-calling path. */

typedef struct { char name[48]; char type[12]; int optional; } ArgSpec;

/* parse "file:int, text:str, [branch:int]" into typed arg specs */
static int parse_arg_sig(const char *sig, ArgSpec *out, int max) {
    int n = 0;
    if (!sig) return 0;
    const char *p = sig;
    while (*p && n < max) {
        while (*p == ' ' || *p == ',') p++;
        if (!*p) break;
        int optional = 0;
        if (*p == '[') { optional = 1; p++; }
        const char *ns = p;
        while (*p && *p != ':' && *p != ']' && *p != ',') p++;
        size_t nlen = (size_t)(p - ns);
        char type[12] = "str";
        if (*p == ':') {
            p++;
            const char *ts = p;
            while (*p && *p != ']' && *p != ',' && *p != ' ') p++;
            size_t tlen = (size_t)(p - ts);
            if (tlen >= sizeof type) tlen = sizeof type - 1;
            memcpy(type, ts, tlen); type[tlen] = 0;
        }
        if (*p == ']') p++;
        if (nlen == 0 || nlen >= sizeof out[0].name) continue;
        memcpy(out[n].name, ns, nlen); out[n].name[nlen] = 0;
        snprintf(out[n].type, sizeof out[n].type, "%s", type);
        out[n].optional = optional;
        n++;
    }
    return n;
}

static const char *type_rule(const char *t) {
    return strcmp(t, "int") == 0 ? "int" : "str";   /* str / unknown -> JSON string */
}

/* emit a GBNF literal for the JSON token  "<s>"  (i.e. the grammar text  "\"<s>\"" ) */
static void gbnf_key(FILE *out, const char *s) {
    fputs("\"\\\"", out); fputs(s, out); fputs("\\\"\"", out);
}

/* args-IDX rule: required args in order, then optional args as a skippable prefix chain */
static void emit_args(FILE *out, size_t idx, const ArgSpec *args, int n) {
    fprintf(out, "args-%zu ::= ", idx);
    int emitted = 0, opens = 0;
    for (int pass = 0; pass < 2; pass++) {           /* pass 0: required, pass 1: optional */
        for (int i = 0; i < n; i++) {
            if (args[i].optional != pass) continue;
            if (pass) { fputs(" ( ", out); opens++; }
            if (emitted) fputs(" \",\" ws ", out);
            gbnf_key(out, args[i].name);
            fprintf(out, " ws \":\" ws %s ws", type_rule(args[i].type));
            emitted++;
        }
    }
    while (opens-- > 0) fputs(" )?", out);
    if (!emitted) fputs("\"\"", out);                /* no args -> empty object body */
    fputs("\n", out);
}

size_t forge_grammar_for_intent(const char *intent, size_t k, FILE *out) {
    if (!out) return 0;
    OpHit hits[20];
    if (k > 20) k = 20;
    size_t n = forge_find_op(intent, hits, k);
    if (n == 0) return 0;

    /* shared lexical rules */
    fputs("ws ::= [ \\t\\n]*\n", out);
    fputs("int ::= \"-\"? [0-9]+\n", out);
    fputs("str ::= ", out); fputs("\"\\\"\"", out);
    fputs(" ( [^\"\\\\] | \"\\\\\" [\"\\\\/bfnrt] )* ", out);
    fputs("\"\\\"\"", out); fputs("\n", out);

    /* root envelope: { "op": <body> } */
    fputs("root ::= ws \"{\" ws ", out); gbnf_key(out, "op");
    fputs(" ws \":\" ws body ws \"}\" ws\n", out);

    fputs("body ::= ", out);
    for (size_t i = 0; i < n; i++) fprintf(out, "%scall-%zu", i ? " | " : "", i);
    fputs("\n", out);

    for (size_t i = 0; i < n; i++) {
        const OpDef *d = forge_op_find(hits[i].name);
        ArgSpec args[32];
        int an = parse_arg_sig(d ? d->arg_sig : "", args, 32);
        fprintf(out, "call-%zu ::= ", i);
        gbnf_key(out, hits[i].name);
        fputs(" ws \",\" ws ", out); gbnf_key(out, "args");
        fprintf(out, " ws \":\" ws \"{\" ws args-%zu ws \"}\"\n", i);
        emit_args(out, i, args, an);
    }
    return n;
}

/* ===================== two more projections of the same op table =====================
 * One source (op table + find_op), four views, guaranteed consistent. The GBNF and
 * JSON Schema views CONSTRAIN generation (per-intent); the catalog and agnostic
 * schema DESCRIBE the surface (whole-set). Derive everything; maintain nothing. */

static void json_str(FILE *out, const char *s) {
    fputc('"', out);
    for (const char *p = s; p && *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '"' || c == '\\') { fputc('\\', out); fputc((int)c, out); }
        else if (c == '\n')        fputs("\\n", out);
        else if (c == '\t')        fputs("\\t", out);
        else if (c < 0x20)         fprintf(out, "\\u%04x", c);
        else                       fputc((int)c, out);
    }
    fputc('"', out);
}
static const char *json_type(const char *t) { return strcmp(t, "int") == 0 ? "integer" : "string"; }

/* JSON Schema: accepts { "op": <const, one of top-k>, "args": { ...typed... } }.
 * The structured-output / strict-tool analog of the scoped GBNF. */
size_t forge_jsonschema_for_intent(const char *intent, size_t k, FILE *out) {
    if (!out) return 0;
    OpHit hits[20];
    if (k > 20) k = 20;
    size_t n = forge_find_op(intent, hits, k);
    if (n == 0) return 0;

    fputs("{\n  \"anyOf\": [\n", out);
    for (size_t i = 0; i < n; i++) {
        const OpDef *d = forge_op_find(hits[i].name);
        ArgSpec args[32];
        int an = parse_arg_sig(d ? d->arg_sig : "", args, 32);
        fputs("    {\n      \"type\": \"object\",\n      \"properties\": {\n", out);
        fputs("        \"op\": { \"const\": ", out); json_str(out, hits[i].name); fputs(" },\n", out);
        fputs("        \"args\": {\n          \"type\": \"object\",\n          \"properties\": {\n", out);
        for (int j = 0; j < an; j++) {
            fputs("            ", out); json_str(out, args[j].name);
            fprintf(out, ": { \"type\": \"%s\" }%s\n", json_type(args[j].type), (j < an - 1) ? "," : "");
        }
        fputs("          },\n          \"required\": [", out);
        int first = 1;
        for (int j = 0; j < an; j++) if (!args[j].optional) {
            if (!first) fputs(", ", out);
            json_str(out, args[j].name); first = 0;
        }
        fputs("],\n          \"additionalProperties\": false\n        }\n", out);
        fputs("      },\n      \"required\": [\"op\", \"args\"],\n      \"additionalProperties\": false\n", out);
        fprintf(out, "    }%s\n", (i < n - 1) ? "," : "");
    }
    fputs("  ]\n}\n", out);
    return n;
}

/* Human catalog (Markdown): the whole set, grouped, with args, an example call,
 * and reversibility — the map a person browses. */
void forge_catalog(FILE *out) {
    if (!out) return;
    size_t N = TABLE_N + g_dyn_n, nm = 0, nq = 0;
    for (size_t i = 0; i < N; i++) { if (op_at(i)->mutates) nm++; else nq++; }
    fprintf(out, "# forge op catalog — %zu operations\n\n", N);
    fputs("Every capability available right now. Mutating ops run in a sandbox and undo atomically.\n\n", out);
    for (int pass = 0; pass < 2; pass++) {            /* 0 = mutating, 1 = query */
        fprintf(out, "## %s ops (%zu)\n\n", pass ? "Query" : "Mutating", pass ? nq : nm);
        for (size_t i = 0; i < N; i++) {
            const OpDef *d = op_at(i);
            if ((d->mutates ? 0 : 1) != pass) continue;
            fprintf(out, "### `%s`\n%s\n\n", d->name, (d->doc && *d->doc) ? d->doc : "_(no description)_");
            ArgSpec args[32];
            int an = parse_arg_sig(d->arg_sig, args, 32);
            if (an > 0) {
                fputs("| arg | type | required |\n|---|---|---|\n", out);
                for (int j = 0; j < an; j++)
                    fprintf(out, "| `%s` | %s | %s |\n", args[j].name, args[j].type, args[j].optional ? "no" : "**yes**");
                fputc('\n', out);
            } else {
                fputs("_No arguments._\n\n", out);
            }
            fprintf(out, "Example: `%s", d->name);
            for (int j = 0; j < an; j++) fprintf(out, " %s=<%s>", args[j].name, args[j].type);
            fputs("`  \n", out);
            if (d->mutates) fputs("Reversible: one undo.\n", out);
            fputc('\n', out);
        }
    }
}

/* Plain-text, reader-agnostic schema: one op per stanza, blank-line separated,
 * fixed `KEY value` lines (ARG/CALL repeat). Parseable by anything that can split
 * lines and whitespace — no JSON, no grammar, no markup that needs a renderer. */
void forge_schema_text(FILE *out) {
    if (!out) return;
    size_t N = TABLE_N + g_dyn_n;
    fprintf(out, "# forge op schema v1 — %zu ops\n", N);
    fputs("# one op per stanza, blank-line separated; lines are `KEY value`; ARG repeats.\n", out);
    fputs("# ARG  <name> <type> required|optional   CALL  <template>\n\n", out);
    for (size_t i = 0; i < N; i++) {
        const OpDef *d = op_at(i);
        fprintf(out, "OP   %s\n", d->name);
        fprintf(out, "KIND %s\n", d->mutates ? "mutate" : "query");
        fprintf(out, "DOC  %s\n", (d->doc && *d->doc) ? d->doc : "");
        ArgSpec args[32];
        int an = parse_arg_sig(d->arg_sig, args, 32);
        for (int j = 0; j < an; j++)
            fprintf(out, "ARG  %s %s %s\n", args[j].name, args[j].type, args[j].optional ? "optional" : "required");
        fprintf(out, "CALL %s", d->name);
        for (int j = 0; j < an; j++) {
            if (args[j].optional) fprintf(out, " [%s=<%s>]", args[j].name, args[j].type);
            else                  fprintf(out, " %s=<%s>", args[j].name, args[j].type);
        }
        fputs("\n\n", out);
    }
}
