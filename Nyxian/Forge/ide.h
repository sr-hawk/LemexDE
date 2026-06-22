/*
 * ide.h — the event-sourced IDE core, multi-file (public C ABI).
 *
 * Same principle as before — the truth is an append-only event log; state is a
 * pure fold of it; undo/redo append inverse events — but the state is now a
 * *project*: a table of files, not one buffer. Every event names the file it
 * acts on, and the file lifecycle (create/delete/rename) is just more events.
 *
 * Files are identified by a stable `FileId` (a slot index). A file's slot is
 * never removed — deleting a file only flips it to "not present" and keeps its
 * bytes. So "delete" drops a *reference*, never frees data; undo re-references
 * it in O(1), losslessly. (Reclaiming the bytes of long-dead files is a separate
 * deliberate GC/compaction pass, not part of editing — same model as git.)
 *
 * `fold(entire log) == live project` remains the one invariant, checked by
 * `ide_verify_fold`.
 */
#ifndef FORGE_IDE_H
#define FORGE_IDE_H

#include <stdint.h>
#include <stddef.h>
#include "hash.h"

typedef uint32_t FileId;

typedef enum {
    EV_CREATE_FILE,   /* path: the file's path */
    EV_DELETE_FILE,   /* path: the file's path at delete time (so it's invertible) */
    EV_RENAME_FILE,   /* path: new path, old_path: old path */
    EV_INSERT,        /* file, offset, text (inserted bytes) */
    EV_DELETE,        /* file, offset, text (removed bytes — kept so it's invertible) */
    EV_PROV,          /* provenance record: text = opaque blob (an AI session/turn); no project effect */
} EventKind;

typedef struct {
    EventKind   kind;
    FileId      file;
    uint32_t    offset;
    uint32_t    len;        /* length of `text` */
    const char *text;       /* INSERT/DELETE: bytes; EV_PROV: the provenance blob */
    const char *path;       /* CREATE/DELETE_FILE: path; RENAME_FILE: new path */
    const char *old_path;   /* RENAME_FILE: old path */
    Hash        source;     /* the EV_PROV (AI turn) that produced this edit; 0 = human */
    Hash        id;         /* content-addressed id over kind+file+offset+len+text+paths */
    uint32_t    group;      /* transient undo-group tag (0 = ungrouped); NOT persisted or hashed —
                             * events an op emits share a group so one undo reverses the whole op */
} Event;

typedef struct IdeCore IdeCore;

IdeCore *ide_create(void);
void     ide_destroy(IdeCore *c);

/* --- File lifecycle (intent in). --- */
FileId ide_create_file(IdeCore *c, const char *path);   /* returns the new file's id */
void   ide_delete_file(IdeCore *c, FileId file);
void   ide_rename_file(IdeCore *c, FileId file, const char *new_path);

/* --- Edits, scoped to a file. --- */
void ide_insert(IdeCore *c, FileId file, uint32_t offset, const char *text);
void ide_delete(IdeCore *c, FileId file, uint32_t offset, uint32_t len);

/* --- History. --- */
void ide_undo(IdeCore *c);
void ide_redo(IdeCore *c);

/* --- Transactions (op-level atomic undo). ---
 * Events committed between begin and end share an undo group, so a single undo
 * reverses the whole op (a composition of primitives), not one primitive. The
 * op layer brackets each op with these; nesting is not supported. */
void ide_group_begin(IdeCore *c);
void ide_group_end(IdeCore *c);
/* (branch-scoped variants declared with the other branch verbs below.) */

/* --- Read-only views for the renderer. --- */
size_t      ide_file_count(const IdeCore *c);                 /* total slots, incl. deleted */
int         ide_file_present(const IdeCore *c, FileId file);  /* 0 if deleted */
const char *ide_file_path(const IdeCore *c, FileId file);
const char *ide_file_text(const IdeCore *c, FileId file, uint32_t *out_len); /* bytes survive deletion */

/* Copy up to `len` bytes starting at `offset` into `out` (must hold `len`).
 * Returns bytes copied. Reads a window straight from the piece table without
 * materializing the whole file — what lets a renderer draw visible lines of a
 * large file cheaply. */
uint32_t ide_file_read(const IdeCore *c, FileId file, uint32_t offset, uint32_t len, char *out);

/* Introspection (tests/debug): how the piece table represents a file. */
size_t ide_debug_piece_count(const IdeCore *c, FileId file);
size_t ide_debug_add_bytes(const IdeCore *c, FileId file); /* total bytes ever inserted (append-only) */

size_t ide_event_count(const IdeCore *c);
int    ide_verify_fold(const IdeCore *c);   /* fold(log) == live project ? */

/* ===================================================================== *
 * Speculative branches — the LLM sandbox.
 *
 * A branch is an independent worktree forked off the current trunk: it gets
 * its own materialized project, its own undo/redo, and its own in-memory
 * (non-durable) event stream. Edits, builds, and runs on a branch never touch
 * trunk. The model can fork, rewrite freely, compile and run, and you either
 *   - ide_branch_merge   : adopt its events onto trunk (they become real,
 *                          logged trunk events), or
 *   - ide_branch_discard : drop it — and trunk is byte-for-byte unchanged.
 * That "discard is a no-op on trunk" property is the sandbox guarantee: you
 * can turn an autonomous model loose and lose nothing if it goes wrong.
 *
 * Branches are transient and never persisted; a crash loses only un-merged
 * speculation, which is correct — speculation isn't durable truth. Merge
 * assumes trunk is unchanged since the fork (the review surface merges
 * promptly); a 3-way merge against a moved trunk is future work.
 * ===================================================================== */
typedef uint32_t BranchId;
#define IDE_NO_BRANCH ((BranchId)0xFFFFFFFFu)

BranchId ide_fork(IdeCore *c);              /* sandbox off the current trunk state */
void     ide_branch_merge(IdeCore *c, BranchId b);   /* adopt onto trunk, then drop */
void     ide_branch_discard(IdeCore *c, BranchId b); /* drop; trunk untouched */
int      ide_branch_valid(const IdeCore *c, BranchId b);

/* Branch-scoped verbs — mirror the trunk verbs exactly (same code path). */
FileId ide_b_create_file(IdeCore *c, BranchId b, const char *path);
void   ide_b_delete_file(IdeCore *c, BranchId b, FileId file);
void   ide_b_rename_file(IdeCore *c, BranchId b, FileId file, const char *new_path);
void   ide_b_insert(IdeCore *c, BranchId b, FileId file, uint32_t offset, const char *text);
void   ide_b_delete(IdeCore *c, BranchId b, FileId file, uint32_t offset, uint32_t len);
void   ide_b_undo(IdeCore *c, BranchId b);
void   ide_b_redo(IdeCore *c, BranchId b);
void   ide_b_group_begin(IdeCore *c, BranchId b);
void   ide_b_group_end(IdeCore *c, BranchId b);

/* The speculative events the branch holds — what the review surface diffs. */
size_t       ide_b_event_count(const IdeCore *c, BranchId b);
const Event *ide_b_event_at(const IdeCore *c, BranchId b, size_t i);

/* ----- Read views (trunk or a branch), for renderer / build / run. -----
 * A view lets a consumer (e.g. the build pipeline) read either trunk or a
 * branch through one interface, so a branch builds with no duplicated code. */
typedef struct { const IdeCore *core; BranchId branch; } IdeView;
IdeView     ide_trunk_view(const IdeCore *c);
IdeView     ide_branch_view(const IdeCore *c, BranchId b);
size_t      ide_view_file_count(IdeView v);
int         ide_view_file_present(IdeView v, FileId file);
const char *ide_view_file_path(IdeView v, FileId file);
const char *ide_view_file_text(IdeView v, FileId file, uint32_t *out_len);

/* ===================================================================== *
 * Provenance — trace every edit to the AI session/turn that made it.
 *
 * Provenance records are append-only events in the same log (EV_PROV); their
 * blob is opaque to the core (the agent layer encodes the session/turn/goal/
 * model-response). Each edit event carries `source` = the id of the EV_PROV it
 * belongs to (0 = a direct human edit). Attribution survives fold, persistence,
 * and branch merges — so any byte in any file traces back to its origin.
 * ===================================================================== */
typedef Hash SourceId;
#define IDE_SOURCE_HUMAN ((SourceId)0)

/* Record a provenance blob; returns its content-addressed id (a SourceId). */
SourceId ide_prov_add(IdeCore *c, const void *data, uint32_t len);
SourceId ide_b_prov_add(IdeCore *c, BranchId b, const void *data, uint32_t len);

/* Attribute subsequent edits to `source` (IDE_SOURCE_HUMAN clears it). */
void ide_set_source(IdeCore *c, SourceId source);
void ide_b_set_source(IdeCore *c, BranchId b, SourceId source);

/* Look up a provenance blob by id in trunk's log. Returns 1 + sets data/len. */
int  ide_prov_find(const IdeCore *c, SourceId id, const char **data, uint32_t *len);

/* The source attributed to the event at `index` in trunk's log. */
SourceId ide_event_source(const IdeCore *c, size_t index);

/* --- Persistence (optional). --- */
/* A core backed by an append-only log file. If a snapshot exists it is loaded
 * first, and only the events appended *after* it are replayed. Pass NULL for
 * snapshot_path to disable snapshots. ide_create() stays for in-memory cores. */
IdeCore *ide_open(const char *log_path, const char *snapshot_path);

/* Write the live project + current log position to the snapshot, so the next
 * ide_open replays only events appended after this point. Returns 1 on success. */
int ide_snapshot(IdeCore *c);

/* How many events had to be replayed on the last ide_open (those after the
 * snapshot). Lets a caller/test confirm the checkpoint actually saved work. */
size_t ide_replayed_count(const IdeCore *c);

#endif /* FORGE_IDE_H */
