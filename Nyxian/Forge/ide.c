#include "ide.h"
#include "arena.h"
#include "piecetable.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- Project: the materialized state — a table of files. Each file's text is
 * a piece table. Slots are never removed; deletion only clears `present`. ---- */
typedef struct {
    char      *path;
    int        present;
    PieceTable doc;
} PFile;

typedef struct {
    PFile *files;
    size_t count, cap;
} Project;

static char *dup_cstr(const char *s) {
    if (!s) return NULL;
    size_t n = strlen(s) + 1;
    char *p = (char *)malloc(n);
    memcpy(p, s, n);
    return p;
}

static void pfile_set_path(PFile *f, const char *path) {
    free(f->path);
    f->path = dup_cstr(path);
}

static void proj_init(Project *p) { p->files = NULL; p->count = 0; p->cap = 0; }
static void proj_ensure(Project *p, FileId id); /* fwd */

/* Materialize a copy of `src` into a fresh `dst`. Each file is copied by its
 * flattened bytes (a fresh piece table), so the branch shares *no* mutable
 * state with the source — the basis of the sandbox. This duplicates the
 * derived materialized cache, never the durable log; the truly heavy thing
 * (compilation) is shared instead via the content-addressed build cache, so a
 * fork is cheap where it counts. (COW piece tables could replace this later
 * behind the same call.) */
static void proj_copy(Project *dst, const Project *src) {
    proj_init(dst);
    if (src->count == 0) return;
    proj_ensure(dst, (FileId)(src->count - 1));
    for (size_t i = 0; i < src->count; i++) {
        const PFile *s = &src->files[i];
        PFile *d = &dst->files[i];
        d->present = s->present;
        free(d->path);
        d->path = dup_cstr(s->path);
        uint32_t dl;
        const char *flat = pt_flat((PieceTable *)&s->doc, &dl); /* bytes survive deletion */
        if (dl) pt_from_bytes(&d->doc, flat, dl);
    }
}
static void proj_free(Project *p) {
    for (size_t i = 0; i < p->count; i++) { free(p->files[i].path); pt_free(&p->files[i].doc); }
    free(p->files);
    proj_init(p);
}

static void proj_ensure(Project *p, FileId id) {
    size_t need = (size_t)id + 1;
    if (need <= p->count) return;
    if (need > p->cap) {
        size_t cap = p->cap ? p->cap : 8;
        while (cap < need) cap *= 2;
        p->files = (PFile *)realloc(p->files, cap * sizeof(PFile));
        p->cap = cap;
    }
    for (size_t i = p->count; i < need; i++) {
        p->files[i].path = NULL;
        p->files[i].present = 0;
        pt_init(&p->files[i].doc);
    }
    p->count = need;
}

static void proj_apply(Project *p, const Event *e) {
    switch (e->kind) {
    case EV_CREATE_FILE: {              /* fresh id => new file; existing id => un-delete */
        proj_ensure(p, e->file);
        PFile *f = &p->files[e->file];
        f->present = 1;
        pfile_set_path(f, e->path);
        break;
    }
    case EV_DELETE_FILE:
        if (e->file < p->count) p->files[e->file].present = 0; /* bytes kept */
        break;
    case EV_RENAME_FILE:
        if (e->file < p->count) pfile_set_path(&p->files[e->file], e->path);
        break;
    case EV_INSERT:
        if (e->file < p->count) pt_insert(&p->files[e->file].doc, e->offset, e->text, e->len);
        break;
    case EV_DELETE:
        if (e->file < p->count) pt_delete(&p->files[e->file].doc, e->offset, e->len);
        break;
    case EV_PROV:           /* provenance metadata — no effect on project state */
        break;
    }
}

/* ---- Append-only log + transient undo/redo stacks. ---- */
typedef struct { Event *items; size_t count, cap; } EventVec;

static void vec_push(EventVec *v, Event e) {
    if (v->count == v->cap) {
        v->cap = v->cap ? v->cap * 2 : 64;
        v->items = (Event *)realloc(v->items, v->cap * sizeof(Event));
    }
    v->items[v->count++] = e;
}
static int vec_pop(EventVec *v, Event *out) {
    if (v->count == 0) return 0;
    *out = v->items[--v->count];
    return 1;
}

/* A speculative branch: a full worktree (its own materialized project, its own
 * undo/redo, its own in-memory event stream) that shares only the arena and the
 * content-addressed build cache with trunk. */
typedef struct {
    int      in_use;
    Project  proj;
    EventVec undo, redo;
    EventVec events;        /* speculative log: fold(events) == proj */
    uint32_t base_events;   /* trunk log.count at fork time (provenance) */
    Hash     current_source; /* edits committed here are attributed to this AI turn */
    uint32_t group_seq, cur_group;  /* undo-group state (see WT) */
} Branch;

struct IdeCore {
    Arena    arena;
    EventVec log;
    Project  proj;
    EventVec undo;
    EventVec redo;
    Hash     current_source;     /* edits on trunk are attributed to this (0 = human) */
    uint32_t group_seq, cur_group;  /* undo-group state (see WT) */

    Branch  *branches;          /* slot table; freed slots are reused */
    size_t   branch_count, branch_cap;

    FILE  *log_file;
    char  *log_path;
    char  *snapshot_path;
    size_t replayed_count;
};

static const char *arena_str(IdeCore *c, const char *s) {
    if (!s) return NULL;
    return arena_copy(&c->arena, s, strlen(s));
}

static Hash event_hash(const Event *e) {
    Hash h = hash_bytes(&e->kind, sizeof e->kind);
    h = hash_combine(h, hash_bytes(&e->file, sizeof e->file));
    h = hash_combine(h, hash_bytes(&e->offset, sizeof e->offset));
    h = hash_combine(h, hash_bytes(&e->len, sizeof e->len));
    if (e->text)     h = hash_combine(h, hash_bytes(e->text, e->len));
    if (e->path)     h = hash_combine(h, hash_str(e->path));
    if (e->old_path) h = hash_combine(h, hash_str(e->old_path));
    return h;
}

static Event event_inverse(const Event *e) {
    Event inv = *e;
    switch (e->kind) {
    case EV_CREATE_FILE: inv.kind = EV_DELETE_FILE; break;
    case EV_DELETE_FILE: inv.kind = EV_CREATE_FILE; break;
    case EV_RENAME_FILE: inv.path = e->old_path; inv.old_path = e->path; break;
    case EV_INSERT:      inv.kind = EV_DELETE; break;
    case EV_DELETE:      inv.kind = EV_INSERT; break;
    case EV_PROV:        break;   /* never inverted (not an edit) */
    }
    inv.id = event_hash(&inv);
    return inv;
}

/* ---- Serialization (one self-describing binary record per event). ---- */
#define SNAP_MAGIC 0x46534E31u /* "FSN1" */

static void wr(FILE *f, const void *p, size_t n) { size_t w = fwrite(p, 1, n, f); (void)w; }
static int  rd(FILE *f, void *p, size_t n)       { return fread(p, 1, n, f) == n; }

static int rd_blob(IdeCore *c, FILE *f, const char **out, uint32_t *out_len) {
    uint32_t n;
    if (!rd(f, &n, sizeof n)) return 0;
    if (n == 0) { *out = NULL; *out_len = 0; return 1; }
    char *p = (char *)arena_alloc(&c->arena, n + 1);
    if (!p || !rd(f, p, n)) return 0;
    p[n] = '\0';
    *out = p;
    *out_len = n;
    return 1;
}

static void wr_blob(FILE *f, const void *p, uint32_t n) {
    wr(f, &n, sizeof n);
    if (n) wr(f, p, n);
}

static void write_event(FILE *f, const Event *e) {
    uint8_t k = (uint8_t)e->kind;
    wr(f, &k, 1);
    wr(f, &e->file, sizeof e->file);
    wr(f, &e->offset, sizeof e->offset);
    wr_blob(f, e->text, e->text ? e->len : 0);
    wr_blob(f, e->path, e->path ? (uint32_t)strlen(e->path) : 0);
    wr_blob(f, e->old_path, e->old_path ? (uint32_t)strlen(e->old_path) : 0);
    wr(f, &e->source, sizeof e->source);
}

static int read_event(IdeCore *c, FILE *f, Event *e) {
    uint8_t k;
    if (!rd(f, &k, 1)) return 0; /* clean EOF */
    e->kind = (EventKind)k;
    if (!rd(f, &e->file, sizeof e->file)) return 0;
    if (!rd(f, &e->offset, sizeof e->offset)) return 0;
    uint32_t tl, pl, ol;
    if (!rd_blob(c, f, &e->text, &tl)) return 0;
    e->len = tl;
    if (!rd_blob(c, f, &e->path, &pl)) return 0;
    if (!rd_blob(c, f, &e->old_path, &ol)) return 0;
    if (!rd(f, &e->source, sizeof e->source)) return 0;
    e->id = event_hash(e);
    e->group = 0;   /* grouping is transient; replayed events are ungrouped */
    return 1;
}

static void log_record(IdeCore *c, Event e) {
    vec_push(&c->log, e);
    if (c->log_file) {
        write_event(c->log_file, &e);
        fflush(c->log_file);
    }
}

/* ---- Worktree: the one edit machinery shared by trunk and every branch.
 * A worktree is (materialized project + undo/redo + an event sink). The sink is
 * the durable trunk log for trunk, or a branch's in-memory speculative stream
 * for a branch — the only thing that differs between them. ---- */
typedef struct {
    Project  *proj;
    EventVec *undo;
    EventVec *redo;
    EventVec *events;   /* branch speculative log; NULL => trunk (durable file) */
    Hash     *source;   /* current provenance source for edits committed here */
    uint32_t *group_seq;  /* monotonic group id source */
    uint32_t *cur_group;  /* group stamped on commits now (0 = none) */
} WT;

static void wt_record(IdeCore *c, WT *w, Event e) {
    if (w->events) vec_push(w->events, e); /* branch: in-memory only, not persisted */
    else           log_record(c, e);       /* trunk: append to log + flush to disk */
}

static void wt_commit(IdeCore *c, WT *w, Event e) {
    /* Attribute to the current source. Preserve a source already on the event
     * (so a branch edit keeps its origin when re-committed onto trunk at merge). */
    if (e.source == 0 && w->source) e.source = *w->source;
    e.group = w->cur_group ? *w->cur_group : 0;   /* transient; not in the content hash */
    e.id = event_hash(&e);
    wt_record(c, w, e);
    proj_apply(w->proj, &e);
    vec_push(w->undo, e);
    w->redo->count = 0;
}

static FileId wt_create_file(IdeCore *c, WT *w, const char *path) {
    FileId id = (FileId)w->proj->count;
    Event e = {0};
    e.kind = EV_CREATE_FILE;
    e.file = id;
    e.path = arena_str(c, path);
    wt_commit(c, w, e);
    return id;
}

static void wt_delete_file(IdeCore *c, WT *w, FileId file) {
    if (file >= w->proj->count || !w->proj->files[file].present) return;
    Event e = {0};
    e.kind = EV_DELETE_FILE;
    e.file = file;
    e.path = arena_str(c, w->proj->files[file].path);
    wt_commit(c, w, e);
}

static void wt_rename_file(IdeCore *c, WT *w, FileId file, const char *new_path) {
    if (file >= w->proj->count || !w->proj->files[file].present) return;
    Event e = {0};
    e.kind = EV_RENAME_FILE;
    e.file = file;
    e.path = arena_str(c, new_path);
    e.old_path = arena_str(c, w->proj->files[file].path);
    wt_commit(c, w, e);
}

static void wt_insert(IdeCore *c, WT *w, FileId file, uint32_t offset, const char *text) {
    if (file >= w->proj->count || !w->proj->files[file].present) return;
    if (offset > pt_len(&w->proj->files[file].doc)) return;
    Event e = {0};
    e.kind = EV_INSERT;
    e.file = file;
    e.offset = offset;
    e.len = (uint32_t)strlen(text);
    e.text = arena_copy(&c->arena, text, e.len);
    wt_commit(c, w, e);
}

static void wt_delete(IdeCore *c, WT *w, FileId file, uint32_t offset, uint32_t len) {
    if (file >= w->proj->count || !w->proj->files[file].present) return;
    PieceTable *d = &w->proj->files[file].doc;
    if (offset + len > pt_len(d)) return;
    char *removed = (char *)malloc(len ? len : 1);  /* captured so DELETE is invertible */
    pt_read(d, offset, len, removed);
    Event e = {0};
    e.kind = EV_DELETE;
    e.file = file;
    e.offset = offset;
    e.len = len;
    e.text = arena_copy(&c->arena, removed, len);
    free(removed);
    wt_commit(c, w, e);
}

/* Undo a whole group atomically: reverse the top event, then keep reversing
 * while the next event shares its (nonzero) group. Ungrouped events (group 0)
 * reverse one at a time, as before. */
static void wt_undo(IdeCore *c, WT *w) {
    Event e;
    if (!vec_pop(w->undo, &e)) return;
    uint32_t g = e.group;
    for (;;) {
        Event inv = event_inverse(&e);   /* undo is an *appended* inverse, never a mutation */
        wt_record(c, w, inv);
        proj_apply(w->proj, &inv);
        vec_push(w->redo, e);
        if (g == 0 || w->undo->count == 0) break;
        if (w->undo->items[w->undo->count - 1].group != g) break;
        vec_pop(w->undo, &e);
    }
}

static void wt_redo(IdeCore *c, WT *w) {
    Event e;
    if (!vec_pop(w->redo, &e)) return;
    uint32_t g = e.group;
    for (;;) {
        wt_record(c, w, e);
        proj_apply(w->proj, &e);
        vec_push(w->undo, e);
        if (g == 0 || w->redo->count == 0) break;
        if (w->redo->items[w->redo->count - 1].group != g) break;
        vec_pop(w->redo, &e);
    }
}

static WT trunk_wt(IdeCore *c) {
    WT w = { &c->proj, &c->undo, &c->redo, NULL, &c->current_source,
             &c->group_seq, &c->cur_group };
    return w;
}

static void branch_free(Branch *br); /* fwd: used by ide_destroy */

IdeCore *ide_create(void) {
    IdeCore *c = (IdeCore *)calloc(1, sizeof(IdeCore));
    c->arena = arena_make((size_t)1 << 20);
    proj_init(&c->proj);
    return c;
}

void ide_destroy(IdeCore *c) {
    if (!c) return;
    if (c->log_file) fclose(c->log_file);
    free(c->log_path);
    free(c->snapshot_path);
    arena_free(&c->arena);
    free(c->log.items);
    free(c->undo.items);
    free(c->redo.items);
    for (size_t i = 0; i < c->branch_count; i++)
        if (c->branches[i].in_use) branch_free(&c->branches[i]);
    free(c->branches);
    proj_free(&c->proj);
    free(c);
}

/* Trunk verbs = the shared worktree machinery aimed at the durable trunk. */
FileId ide_create_file(IdeCore *c, const char *path) { WT w = trunk_wt(c); return wt_create_file(c, &w, path); }
void   ide_delete_file(IdeCore *c, FileId f)                  { WT w = trunk_wt(c); wt_delete_file(c, &w, f); }
void   ide_rename_file(IdeCore *c, FileId f, const char *np)  { WT w = trunk_wt(c); wt_rename_file(c, &w, f, np); }
void   ide_insert(IdeCore *c, FileId f, uint32_t o, const char *t)  { WT w = trunk_wt(c); wt_insert(c, &w, f, o, t); }
void   ide_delete(IdeCore *c, FileId f, uint32_t o, uint32_t l)     { WT w = trunk_wt(c); wt_delete(c, &w, f, o, l); }
void   ide_undo(IdeCore *c) { WT w = trunk_wt(c); wt_undo(c, &w); }
void   ide_redo(IdeCore *c) { WT w = trunk_wt(c); wt_redo(c, &w); }

void ide_group_begin(IdeCore *c) { if (!c->cur_group) c->cur_group = ++c->group_seq; }
void ide_group_end(IdeCore *c)   { c->cur_group = 0; }

/* ---- Speculative branches ---- */

static int branch_ok(const IdeCore *c, BranchId b) {
    return b < c->branch_count && c->branches[b].in_use;
}

static void branch_free(Branch *br) {
    proj_free(&br->proj);
    free(br->undo.items);
    free(br->redo.items);
    free(br->events.items);
    memset(br, 0, sizeof *br);  /* clears in_use; event text stays in the arena */
}

int ide_branch_valid(const IdeCore *c, BranchId b) { return branch_ok(c, b); }

BranchId ide_fork(IdeCore *c) {
    BranchId id = (BranchId)c->branch_count;
    for (size_t i = 0; i < c->branch_count; i++)
        if (!c->branches[i].in_use) { id = (BranchId)i; break; } /* reuse a freed slot */

    if (id == (BranchId)c->branch_count) {
        if (c->branch_count == c->branch_cap) {
            c->branch_cap = c->branch_cap ? c->branch_cap * 2 : 4;
            c->branches = (Branch *)realloc(c->branches, c->branch_cap * sizeof(Branch));
        }
        c->branch_count++;
    }
    Branch *br = &c->branches[id];
    memset(br, 0, sizeof *br);
    br->in_use = 1;
    br->base_events = (uint32_t)c->log.count;
    proj_copy(&br->proj, &c->proj);   /* snapshot trunk into the sandbox */
    return id;
}

void ide_branch_discard(IdeCore *c, BranchId b) {
    if (!branch_ok(c, b)) return;
    branch_free(&c->branches[b]);     /* trunk never touched — the sandbox guarantee */
}

void ide_branch_merge(IdeCore *c, BranchId b) {
    if (!branch_ok(c, b)) return;
    Branch *br = &c->branches[b];
    WT t = trunk_wt(c);
    /* Replay the branch's speculative events as real, logged trunk events.
     * (Assumes trunk is unchanged since the fork; FileIds then line up.) */
    for (size_t i = 0; i < br->events.count; i++)
        wt_commit(c, &t, br->events.items[i]);
    branch_free(br);
}

static WT branch_wt(IdeCore *c, BranchId b) {
    Branch *br = &c->branches[b];
    WT w = { &br->proj, &br->undo, &br->redo, &br->events, &br->current_source,
             &br->group_seq, &br->cur_group };
    return w;
}

FileId ide_b_create_file(IdeCore *c, BranchId b, const char *path) {
    if (!branch_ok(c, b)) return IDE_NO_BRANCH;
    WT w = branch_wt(c, b); return wt_create_file(c, &w, path);
}
void ide_b_delete_file(IdeCore *c, BranchId b, FileId f)                 { if (!branch_ok(c, b)) return; WT w = branch_wt(c, b); wt_delete_file(c, &w, f); }
void ide_b_rename_file(IdeCore *c, BranchId b, FileId f, const char *np) { if (!branch_ok(c, b)) return; WT w = branch_wt(c, b); wt_rename_file(c, &w, f, np); }
void ide_b_insert(IdeCore *c, BranchId b, FileId f, uint32_t o, const char *t) { if (!branch_ok(c, b)) return; WT w = branch_wt(c, b); wt_insert(c, &w, f, o, t); }
void ide_b_delete(IdeCore *c, BranchId b, FileId f, uint32_t o, uint32_t l)    { if (!branch_ok(c, b)) return; WT w = branch_wt(c, b); wt_delete(c, &w, f, o, l); }
void ide_b_undo(IdeCore *c, BranchId b) { if (!branch_ok(c, b)) return; WT w = branch_wt(c, b); wt_undo(c, &w); }
void ide_b_redo(IdeCore *c, BranchId b) { if (!branch_ok(c, b)) return; WT w = branch_wt(c, b); wt_redo(c, &w); }
void ide_b_group_begin(IdeCore *c, BranchId b) { if (!branch_ok(c, b)) return; Branch *br = &c->branches[b]; if (!br->cur_group) br->cur_group = ++br->group_seq; }
void ide_b_group_end(IdeCore *c, BranchId b)   { if (!branch_ok(c, b)) return; c->branches[b].cur_group = 0; }

size_t ide_b_event_count(const IdeCore *c, BranchId b) {
    return branch_ok(c, b) ? c->branches[b].events.count : 0;
}
const Event *ide_b_event_at(const IdeCore *c, BranchId b, size_t i) {
    if (!branch_ok(c, b) || i >= c->branches[b].events.count) return NULL;
    return &c->branches[b].events.items[i];
}

/* ---- Read views (trunk or branch) ---- */

static const Project *view_proj(IdeView v) {
    if (v.branch == IDE_NO_BRANCH) return &v.core->proj;
    if (!branch_ok(v.core, v.branch)) return NULL;
    return &v.core->branches[v.branch].proj;
}

IdeView ide_trunk_view(const IdeCore *c)            { IdeView v = { c, IDE_NO_BRANCH }; return v; }
IdeView ide_branch_view(const IdeCore *c, BranchId b) { IdeView v = { c, b }; return v; }

size_t ide_view_file_count(IdeView v) {
    const Project *p = view_proj(v);
    return p ? p->count : 0;
}
int ide_view_file_present(IdeView v, FileId file) {
    const Project *p = view_proj(v);
    return p && file < p->count && p->files[file].present;
}
const char *ide_view_file_path(IdeView v, FileId file) {
    const Project *p = view_proj(v);
    if (!p || file >= p->count || !p->files[file].path) return "";
    return p->files[file].path;
}
const char *ide_view_file_text(IdeView v, FileId file, uint32_t *out_len) {
    const Project *p = view_proj(v);
    if (!p || file >= p->count) { if (out_len) *out_len = 0; return ""; }
    PieceTable *d = (PieceTable *)&p->files[file].doc; /* logically const: refresh cache */
    return pt_flat(d, out_len);
}

/* ---- Provenance ---- */

/* Append an EV_PROV record to a worktree's stream (no project effect, no undo). */
static SourceId wt_prov_add(IdeCore *c, WT *w, const void *data, uint32_t len) {
    Event e = {0};
    e.kind = EV_PROV;
    e.len = len;
    e.text = arena_copy(&c->arena, (const char *)data, len);
    e.source = 0;
    e.id = event_hash(&e);
    wt_record(c, w, e);
    return e.id;
}

SourceId ide_prov_add(IdeCore *c, const void *data, uint32_t len) {
    WT w = trunk_wt(c);
    return wt_prov_add(c, &w, data, len);
}

SourceId ide_b_prov_add(IdeCore *c, BranchId b, const void *data, uint32_t len) {
    if (!branch_ok(c, b)) return IDE_SOURCE_HUMAN;
    WT w = branch_wt(c, b);
    return wt_prov_add(c, &w, data, len);
}

void ide_set_source(IdeCore *c, SourceId source) { c->current_source = source; }

void ide_b_set_source(IdeCore *c, BranchId b, SourceId source) {
    if (branch_ok(c, b)) c->branches[b].current_source = source;
}

int ide_prov_find(const IdeCore *c, SourceId id, const char **data, uint32_t *len) {
    for (size_t i = 0; i < c->log.count; i++) {
        const Event *e = &c->log.items[i];
        if (e->kind == EV_PROV && e->id == id) {
            if (data) *data = e->text;
            if (len)  *len = e->len;
            return 1;
        }
    }
    return 0;
}

SourceId ide_event_source(const IdeCore *c, size_t index) {
    return index < c->log.count ? c->log.items[index].source : IDE_SOURCE_HUMAN;
}

size_t ide_file_count(const IdeCore *c) { return c->proj.count; }

int ide_file_present(const IdeCore *c, FileId file) {
    return file < c->proj.count && c->proj.files[file].present;
}

const char *ide_file_path(const IdeCore *c, FileId file) {
    if (file >= c->proj.count || !c->proj.files[file].path) return "";
    return c->proj.files[file].path;
}

const char *ide_file_text(const IdeCore *c, FileId file, uint32_t *out_len) {
    if (file >= c->proj.count) { if (out_len) *out_len = 0; return ""; }
    /* logically const: pt_flat only refreshes a transparent cache */
    PieceTable *d = (PieceTable *)&c->proj.files[file].doc;
    return pt_flat(d, out_len);
}

uint32_t ide_file_read(const IdeCore *c, FileId file, uint32_t offset, uint32_t len, char *out) {
    if (file >= c->proj.count) return 0;
    const PieceTable *d = &c->proj.files[file].doc;
    if (offset >= pt_len(d)) return 0;
    if (offset + len > pt_len(d)) len = pt_len(d) - offset;
    return pt_read(d, offset, len, out);
}

size_t ide_event_count(const IdeCore *c) { return c->log.count; }
size_t ide_replayed_count(const IdeCore *c) { return c->replayed_count; }

size_t ide_debug_piece_count(const IdeCore *c, FileId file) {
    return file < c->proj.count ? pt_node_count(&c->proj.files[file].doc) : 0;
}
size_t ide_debug_add_bytes(const IdeCore *c, FileId file) {
    return file < c->proj.count ? c->proj.files[file].doc.add_len : 0;
}

static int doc_equal(PieceTable *a, PieceTable *b) {
    uint32_t la, lb;
    const char *ta = pt_flat(a, &la);
    const char *tb = pt_flat(b, &lb);
    return la == lb && (la == 0 || memcmp(ta, tb, la) == 0);
}
static int path_equal(const char *a, const char *b) {
    if (!a) a = "";
    if (!b) b = "";
    return strcmp(a, b) == 0;
}

int ide_verify_fold(const IdeCore *c) {
    Project fresh;
    proj_init(&fresh);
    for (size_t i = 0; i < c->log.count; i++) proj_apply(&fresh, &c->log.items[i]);

    int ok = (fresh.count == c->proj.count);
    for (size_t i = 0; ok && i < fresh.count; i++) {
        ok = fresh.files[i].present == c->proj.files[i].present
          && path_equal(fresh.files[i].path, c->proj.files[i].path)
          && doc_equal(&fresh.files[i].doc, (PieceTable *)&c->proj.files[i].doc);
    }
    proj_free(&fresh);
    return ok;
}

/* ---- Snapshots ---- */

int ide_snapshot(IdeCore *c) {
    if (!c->snapshot_path) return 0;
    FILE *f = fopen(c->snapshot_path, "wb");
    if (!f) return 0;
    uint32_t magic = SNAP_MAGIC;
    uint64_t baked = c->log.count;
    uint32_t fc = (uint32_t)c->proj.count;
    wr(f, &magic, sizeof magic);
    wr(f, &baked, sizeof baked);
    wr(f, &fc, sizeof fc);
    for (size_t i = 0; i < c->proj.count; i++) {
        PFile *pf = &c->proj.files[i];
        uint8_t present = (uint8_t)pf->present;
        uint32_t dl;
        const char *flat = pt_flat(&pf->doc, &dl);     /* snapshot stores flat content */
        wr(f, &present, 1);
        wr_blob(f, pf->path, pf->path ? (uint32_t)strlen(pf->path) : 0);
        wr_blob(f, flat, dl);
    }
    fclose(f);
    return 1;
}

static uint64_t snapshot_load(IdeCore *c) {
    if (!c->snapshot_path) return 0;
    FILE *f = fopen(c->snapshot_path, "rb");
    if (!f) return 0;
    uint32_t magic, fc;
    uint64_t baked;
    if (!rd(f, &magic, sizeof magic) || magic != SNAP_MAGIC ||
        !rd(f, &baked, sizeof baked) || !rd(f, &fc, sizeof fc)) {
        fclose(f);
        return 0;
    }
    if (fc > 0) proj_ensure(&c->proj, (FileId)(fc - 1));
    for (uint32_t i = 0; i < fc; i++) {
        PFile *pf = &c->proj.files[i];
        uint8_t present;
        uint32_t pl, dl;
        if (!rd(f, &present, 1) || !rd(f, &pl, sizeof pl)) { fclose(f); return 0; }
        pf->present = present;
        if (pl) {
            char *p = (char *)malloc(pl + 1);
            if (!rd(f, p, pl)) { free(p); fclose(f); return 0; }
            p[pl] = '\0';
            free(pf->path);
            pf->path = p;
        }
        if (!rd(f, &dl, sizeof dl)) { fclose(f); return 0; }
        if (dl) {
            char *tmp = (char *)malloc(dl);
            if (!rd(f, tmp, dl)) { free(tmp); fclose(f); return 0; }
            pt_from_bytes(&pf->doc, tmp, dl);          /* restored content = one original piece */
            free(tmp);
        }
    }
    fclose(f);
    return baked;
}

IdeCore *ide_open(const char *log_path, const char *snapshot_path) {
    IdeCore *c = ide_create();
    c->log_path = dup_cstr(log_path);
    c->snapshot_path = snapshot_path ? dup_cstr(snapshot_path) : NULL;

    uint64_t baked = snapshot_load(c);

    FILE *rf = fopen(log_path, "rb");
    if (rf) {
        Event e;
        uint64_t idx = 0;
        while (read_event(c, rf, &e)) {
            vec_push(&c->log, e);
            if (idx >= baked) { proj_apply(&c->proj, &e); c->replayed_count++; }
            idx++;
        }
        fclose(rf);
    }

    c->log_file = fopen(log_path, "ab");
    return c;
}
