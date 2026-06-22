/*
 * arena.h — forward-only bump allocator, chained so it never runs out.
 *
 * Allocation is a single forward pass: bump a pointer in the current block;
 * when a block fills, prepend a fresh one. Blocks never move or get freed
 * individually, so every pointer the arena ever handed out stays valid for the
 * arena's lifetime — which is exactly what the append-only event/text stores
 * need. Reclaim everything at once with reset/free.
 */
#ifndef FORGE_ARENA_H
#define FORGE_ARENA_H

#include <stddef.h>

typedef struct ArenaBlock ArenaBlock;

typedef struct {
    ArenaBlock *head;        /* most-recent block */
    size_t      block_size;  /* default size for new blocks */
} Arena;

Arena arena_make(size_t block_size);
void  arena_free(Arena *a);
void  arena_reset(Arena *a);                       /* drop all blocks */
void *arena_alloc(Arena *a, size_t n);             /* 16-byte aligned; grows as needed */
char *arena_copy(Arena *a, const char *s, size_t len);  /* copy len bytes + NUL */

#endif /* FORGE_ARENA_H */
