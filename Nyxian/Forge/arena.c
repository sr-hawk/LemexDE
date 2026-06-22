#include "arena.h"

#include <stdlib.h>
#include <string.h>

struct ArenaBlock {
    ArenaBlock   *next;
    size_t        cap, used;
    unsigned char data[];   /* flexible array member */
};

static ArenaBlock *block_new(size_t cap) {
    ArenaBlock *b = (ArenaBlock *)malloc(sizeof(ArenaBlock) + cap);
    b->next = NULL;
    b->cap = cap;
    b->used = 0;
    return b;
}

Arena arena_make(size_t block_size) {
    Arena a;
    a.head = NULL;
    a.block_size = block_size ? block_size : (1u << 16);
    return a;
}

void arena_free(Arena *a) {
    ArenaBlock *b = a->head;
    while (b) { ArenaBlock *next = b->next; free(b); b = next; }
    a->head = NULL;
}

void arena_reset(Arena *a) { arena_free(a); }

void *arena_alloc(Arena *a, size_t n) {
    if (a->head) {
        size_t aligned = (a->head->used + 15u) & ~(size_t)15u;
        if (aligned + n <= a->head->cap) {
            void *p = a->head->data + aligned;
            a->head->used = aligned + n;
            return p;
        }
    }
    /* need a new block: at least block_size, but big enough for this request */
    size_t cap = a->block_size < n ? n : a->block_size;
    ArenaBlock *b = block_new(cap);
    b->next = a->head;
    a->head = b;
    b->used = n;
    return b->data;
}

char *arena_copy(Arena *a, const char *s, size_t len) {
    char *p = (char *)arena_alloc(a, len + 1);
    memcpy(p, s, len);
    p[len] = '\0';
    return p;
}
