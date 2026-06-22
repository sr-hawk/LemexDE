/*
 * piecetable.h — a file's text as an append-only piece *tree*.
 *
 * Same model as a piece table: two immutable byte stores — `original` and an
 * append-only `add` buffer — with the document expressed as an ordered set of
 * spans into them. Editing never moves or erases bytes; it only re-arranges
 * spans, and deleted bytes stay put, unreferenced.
 *
 * The spans live in a position-ordered **treap** (a randomized balanced BST)
 * whose nodes are augmented with subtree byte-lengths. That gives O(log n)
 * offset->span lookup, and insert/delete reduce to the treap's split/merge by
 * position — so a small edit in a huge file is O(log n), not O(pieces) or
 * O(file). Priorities are drawn from a deterministic per-table PRNG, so the same
 * edit sequence builds the same tree (reproducible).
 */
#ifndef FORGE_PIECETABLE_H
#define FORGE_PIECETABLE_H

#include <stdint.h>
#include <stddef.h>

struct PtNode; /* treap node, defined in piecetable.c */

typedef struct {
    char    *original;  uint32_t original_len;      /* immutable initial content */
    char    *add;       uint32_t add_len, add_cap;  /* append-only insert store */
    struct PtNode *root;                            /* length-augmented treap */
    uint32_t prng;                                  /* deterministic node priorities */
    char    *flat;      uint32_t flat_len; int flat_valid; /* lazy whole-text cache */
} PieceTable;

void     pt_init(PieceTable *pt);
void     pt_free(PieceTable *pt);
void     pt_from_bytes(PieceTable *pt, const char *bytes, uint32_t len);

void     pt_insert(PieceTable *pt, uint32_t offset, const char *text, uint32_t len);
void     pt_delete(PieceTable *pt, uint32_t offset, uint32_t len);

uint32_t pt_len(const PieceTable *pt);
uint32_t pt_read(const PieceTable *pt, uint32_t offset, uint32_t len, char *out);
const char *pt_flat(PieceTable *pt, uint32_t *out_len);
uint32_t pt_node_count(const PieceTable *pt);   /* spans currently in the tree */

#endif /* FORGE_PIECETABLE_H */
