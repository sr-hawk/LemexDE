#include "diff.h"

#include <stdlib.h>
#include <string.h>

/* A line is a (start, len) slice into the source buffer; the newline is excluded. */
typedef struct { const char *p; size_t len; } Line;
typedef struct { Line *items; size_t count, cap; } LineVec;

static void lv_push(LineVec *v, const char *p, size_t len) {
    if (v->count == v->cap) {
        v->cap = v->cap ? v->cap * 2 : 64;
        v->items = (Line *)realloc(v->items, v->cap * sizeof(Line));
    }
    v->items[v->count].p = p;
    v->items[v->count].len = len;
    v->count++;
}

/* Split into lines on '\n'. A trailing '\n' does NOT create a final empty line. */
static void split_lines(const char *buf, size_t len, LineVec *out) {
    size_t start = 0;
    for (size_t i = 0; i < len; i++) {
        if (buf[i] == '\n') {
            lv_push(out, buf + start, i - start);
            start = i + 1;
        }
    }
    if (start < len) lv_push(out, buf + start, len - start);
}

static int line_eq(Line a, Line b) {
    return a.len == b.len && (a.len == 0 || memcmp(a.p, b.p, a.len) == 0);
}

/* ---- output accumulator ---- */
typedef struct { DiffLine *items; size_t count, cap; } DiffVec;

static void emit(DiffVec *v, DiffKind kind, Line ln) {
    if (v->count == v->cap) {
        v->cap = v->cap ? v->cap * 2 : 64;
        v->items = (DiffLine *)realloc(v->items, v->cap * sizeof(DiffLine));
    }
    char *s = (char *)malloc(ln.len + 1);
    if (ln.len) memcpy(s, ln.p, ln.len);
    s[ln.len] = '\0';
    v->items[v->count].kind = kind;
    v->items[v->count].text = s;
    v->count++;
}

/* LCS-diff the middle region a[a0..a1) vs b[b0..b1), appending to `out`. */
#define DIFF_CELL_CAP 4000000u  /* cap the DP table; beyond this, coarse replace */

static void diff_middle(const Line *A, size_t a0, size_t a1,
                        const Line *B, size_t b0, size_t b1, DiffVec *out) {
    size_t ma = a1 - a0, mb = b1 - b0;
    if (ma == 0) { for (size_t j = b0; j < b1; j++) emit(out, DIFF_ADDED, B[j]); return; }
    if (mb == 0) { for (size_t i = a0; i < a1; i++) emit(out, DIFF_REMOVED, A[i]); return; }
    if (ma * mb > DIFF_CELL_CAP) {                 /* too big to LCS — coarse replace */
        for (size_t i = a0; i < a1; i++) emit(out, DIFF_REMOVED, A[i]);
        for (size_t j = b0; j < b1; j++) emit(out, DIFF_ADDED, B[j]);
        return;
    }

    size_t w = mb + 1;
    int *dp = (int *)calloc((ma + 1) * w, sizeof(int));
    for (size_t i = 1; i <= ma; i++)
        for (size_t j = 1; j <= mb; j++)
            dp[i * w + j] = line_eq(A[a0 + i - 1], B[b0 + j - 1])
                ? dp[(i - 1) * w + (j - 1)] + 1
                : (dp[(i - 1) * w + j] >= dp[i * w + (j - 1)]
                       ? dp[(i - 1) * w + j] : dp[i * w + (j - 1)]);

    /* backtrack -> reversed list of (kind, line) */
    DiffVec rev = {0};
    size_t i = ma, j = mb;
    while (i > 0 && j > 0) {
        if (line_eq(A[a0 + i - 1], B[b0 + j - 1])) { emit(&rev, DIFF_CONTEXT, A[a0 + i - 1]); i--; j--; }
        /* backtrack runs end->start, so take ADDED first here to get the
         * conventional removed-before-added order once reversed to forward. */
        else if (dp[i * w + (j - 1)] >= dp[(i - 1) * w + j]) { emit(&rev, DIFF_ADDED, B[b0 + j - 1]); j--; }
        else { emit(&rev, DIFF_REMOVED, A[a0 + i - 1]); i--; }
    }
    while (i > 0) { emit(&rev, DIFF_REMOVED, A[a0 + i - 1]); i--; }
    while (j > 0) { emit(&rev, DIFF_ADDED, B[b0 + j - 1]); j--; }
    free(dp);

    /* append in forward order (rev holds it reversed; move the strings over) */
    for (size_t k = rev.count; k > 0; k--) {
        if (out->count == out->cap) {
            out->cap = out->cap ? out->cap * 2 : 64;
            out->items = (DiffLine *)realloc(out->items, out->cap * sizeof(DiffLine));
        }
        out->items[out->count++] = rev.items[k - 1];   /* transfers ownership of .text */
    }
    free(rev.items);
}

Diff forge_diff(const char *a, size_t alen, const char *b, size_t blen) {
    LineVec la = {0}, lb = {0};
    split_lines(a, alen, &la);
    split_lines(b, blen, &lb);

    DiffVec out = {0};

    /* common prefix */
    size_t p = 0;
    while (p < la.count && p < lb.count && line_eq(la.items[p], lb.items[p])) p++;
    /* common suffix (not overlapping the prefix) */
    size_t s = 0;
    while (s < (la.count - p) && s < (lb.count - p)
           && line_eq(la.items[la.count - 1 - s], lb.items[lb.count - 1 - s])) s++;

    for (size_t k = 0; k < p; k++) emit(&out, DIFF_CONTEXT, la.items[k]);
    diff_middle(la.items, p, la.count - s, lb.items, p, lb.count - s, &out);
    for (size_t k = lb.count - s; k < lb.count; k++) emit(&out, DIFF_CONTEXT, lb.items[k]);

    Diff d = { out.items, out.count, 0, 0 };
    for (size_t k = 0; k < out.count; k++) {
        if (out.items[k].kind == DIFF_ADDED) d.added++;
        else if (out.items[k].kind == DIFF_REMOVED) d.removed++;
    }
    free(la.items);
    free(lb.items);
    return d;
}

void forge_diff_free(Diff *d) {
    for (size_t i = 0; i < d->count; i++) free(d->lines[i].text);
    free(d->lines);
    memset(d, 0, sizeof *d);
}
