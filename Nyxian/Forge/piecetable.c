#include "piecetable.h"

#include <stdlib.h>
#include <string.h>

typedef enum { PT_ORIGINAL, PT_ADD } PtBuf;
typedef struct { PtBuf buf; uint32_t start, len; } Piece;

typedef struct PtNode {
    Piece    piece;
    uint32_t subtree_len;   /* slen(left) + piece.len + slen(right) */
    uint32_t prio;          /* treap heap key */
    struct PtNode *left, *right;
} PtNode;

static uint32_t slen(const PtNode *n) { return n ? n->subtree_len : 0; }
static void update(PtNode *n) { if (n) n->subtree_len = slen(n->left) + n->piece.len + slen(n->right); }

/* deterministic xorshift32 — same edit sequence => same tree */
static uint32_t prng_next(PieceTable *pt) {
    uint32_t x = pt->prng ? pt->prng : 0x9e3779b9u;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    pt->prng = x;
    return x;
}

static PtNode *node_new(PieceTable *pt, Piece p) {
    PtNode *n = (PtNode *)malloc(sizeof *n);
    n->piece = p;
    n->left = n->right = NULL;
    n->prio = prng_next(pt);
    n->subtree_len = p.len;
    return n;
}

static void node_free_all(PtNode *n) {
    if (!n) return;
    node_free_all(n->left);
    node_free_all(n->right);
    free(n);
}

/* all of L's positions precede all of R's */
static PtNode *merge(PtNode *L, PtNode *R) {
    if (!L) return R;
    if (!R) return L;
    if (L->prio >= R->prio) {
        L->right = merge(L->right, R);
        update(L);
        return L;
    } else {
        R->left = merge(L, R->left);
        update(R);
        return R;
    }
}

/* split `n` at byte position `pos`: *outL gets the first `pos` bytes, *outR the
 * rest. If `pos` lands inside a node's piece, that piece is cut in two. */
static void split(PieceTable *pt, PtNode *n, uint32_t pos, PtNode **outL, PtNode **outR) {
    if (!n) { *outL = *outR = NULL; return; }
    uint32_t ll = slen(n->left);
    if (pos <= ll) {
        PtNode *LL, *LR;
        split(pt, n->left, pos, &LL, &LR);
        n->left = LR;
        update(n);
        *outL = LL;
        *outR = n;
    } else if (pos >= ll + n->piece.len) {
        PtNode *RL, *RR;
        split(pt, n->right, pos - ll - n->piece.len, &RL, &RR);
        n->right = RL;
        update(n);
        *outL = n;
        *outR = RR;
    } else {
        /* cut this node's piece; re-attach its right subtree via merge so the
         * heap property is preserved (the new node gets a fresh priority). */
        uint32_t intra = pos - ll;
        Piece p = n->piece;
        PtNode *old_right = n->right;
        n->piece.len = intra;
        n->right = NULL;
        update(n);
        PtNode *rp = node_new(pt, (Piece){ p.buf, p.start + intra, p.len - intra });
        *outL = n;
        *outR = merge(rp, old_right);
    }
}

void pt_init(PieceTable *pt) {
    memset(pt, 0, sizeof *pt);
    pt->prng = 0x12345678u;
}

void pt_free(PieceTable *pt) {
    free(pt->original);
    free(pt->add);
    node_free_all(pt->root);
    free(pt->flat);
    pt_init(pt);
}

void pt_from_bytes(PieceTable *pt, const char *bytes, uint32_t len) {
    pt_free(pt);
    if (len == 0) return;
    pt->original = (char *)malloc(len);
    memcpy(pt->original, bytes, len);
    pt->original_len = len;
    pt->root = node_new(pt, (Piece){ PT_ORIGINAL, 0, len });
}

static void add_reserve(PieceTable *pt, uint32_t extra) {
    uint32_t need = pt->add_len + extra;
    if (need <= pt->add_cap) return;
    uint32_t cap = pt->add_cap ? pt->add_cap : 64;
    while (cap < need) cap *= 2;
    pt->add = (char *)realloc(pt->add, cap);
    pt->add_cap = cap;
}

/* If the rightmost piece of `n` is contiguous in the same buffer with
 * [start, start+len), extend it in place (updating subtree lengths on the way
 * back up) and return 1. This coalesces sequential typing into one piece. */
static int extend_rightmost(PtNode *n, PtBuf buf, uint32_t start, uint32_t len) {
    if (!n) return 0;
    int done;
    if (n->right) {
        done = extend_rightmost(n->right, buf, start, len);
    } else {
        done = (n->piece.buf == buf && n->piece.start + n->piece.len == start);
        if (done) n->piece.len += len;
    }
    if (done) n->subtree_len += len;
    return done;
}

void pt_insert(PieceTable *pt, uint32_t offset, const char *text, uint32_t len) {
    if (len == 0) return;
    uint32_t add_start = pt->add_len;
    add_reserve(pt, len);
    memcpy(pt->add + add_start, text, len);
    pt->add_len += len;

    PtNode *L, *R;
    split(pt, pt->root, offset, &L, &R);
    /* coalesce with the piece just before the cut when the bytes are contiguous */
    if (L && extend_rightmost(L, PT_ADD, add_start, len)) {
        pt->root = merge(L, R);
    } else {
        PtNode *mid = node_new(pt, (Piece){ PT_ADD, add_start, len });
        pt->root = merge(merge(L, mid), R);
    }
    pt->flat_valid = 0;
}

void pt_delete(PieceTable *pt, uint32_t offset, uint32_t len) {
    if (len == 0) return;
    PtNode *L, *M, *cut, *R;
    split(pt, pt->root, offset, &L, &M);
    split(pt, M, len, &cut, &R);   /* `cut` is exactly the deleted span */
    node_free_all(cut);
    pt->root = merge(L, R);
    pt->flat_valid = 0;
}

uint32_t pt_len(const PieceTable *pt) { return slen(pt->root); }

/* copy the window [d0,d1) in-order, skipping whole subtrees that precede it */
static void read_rec(const PieceTable *pt, const PtNode *n, uint32_t *pos,
                     uint32_t d0, uint32_t d1, char *out, uint32_t *w) {
    if (!n || *pos >= d1) return;
    uint32_t ll = slen(n->left);
    if (*pos + ll <= d0) {
        *pos += ll;            /* entire left subtree is before the window */
    } else {
        read_rec(pt, n->left, pos, d0, d1, out, w);
    }
    if (*pos >= d1) return;
    uint32_t g0 = *pos, g1 = *pos + n->piece.len;
    if (g1 > d0 && g0 < d1) {
        uint32_t s = d0 > g0 ? d0 : g0;
        uint32_t e = d1 < g1 ? d1 : g1;
        const char *base = (n->piece.buf == PT_ORIGINAL) ? pt->original : pt->add;
        memcpy(out + *w, base + n->piece.start + (s - g0), e - s);
        *w += e - s;
    }
    *pos = g1;
    read_rec(pt, n->right, pos, d0, d1, out, w);
}

uint32_t pt_read(const PieceTable *pt, uint32_t offset, uint32_t len, char *out) {
    uint32_t pos = 0, w = 0;
    read_rec(pt, pt->root, &pos, offset, offset + len, out, &w);
    return w;
}

static void flat_rec(const PieceTable *pt, const PtNode *n, char *out, uint32_t *w) {
    if (!n) return;
    flat_rec(pt, n->left, out, w);
    const char *base = (n->piece.buf == PT_ORIGINAL) ? pt->original : pt->add;
    memcpy(out + *w, base + n->piece.start, n->piece.len);
    *w += n->piece.len;
    flat_rec(pt, n->right, out, w);
}

const char *pt_flat(PieceTable *pt, uint32_t *out_len) {
    if (!pt->flat_valid) {
        uint32_t total = pt_len(pt);
        free(pt->flat);
        pt->flat = (char *)malloc(total + 1);
        uint32_t w = 0;
        flat_rec(pt, pt->root, pt->flat, &w);
        pt->flat[w] = '\0';
        pt->flat_len = w;
        pt->flat_valid = 1;
    }
    if (out_len) *out_len = pt->flat_len;
    return pt->flat;
}

static uint32_t count_rec(const PtNode *n) { return n ? 1 + count_rec(n->left) + count_rec(n->right) : 0; }
uint32_t pt_node_count(const PieceTable *pt) { return count_rec(pt->root); }
