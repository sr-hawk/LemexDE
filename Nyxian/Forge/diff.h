/*
 * diff.h — a line-based text diff, for the review surface.
 *
 * When the agent rewrites a file in its sandbox, the human reviewing it wants to
 * see "- old line / + new line", not a byte-offset event. `forge_diff` produces
 * an ordered list of context/removed/added lines (old `a` vs new `b`). Common
 * prefix/suffix lines are trimmed to context and only the differing middle is
 * LCS-diffed, so a localized edit stays cheap.
 */
#ifndef FORGE_DIFF_H
#define FORGE_DIFF_H

#include <stddef.h>

typedef enum { DIFF_CONTEXT = 0, DIFF_REMOVED = -1, DIFF_ADDED = 1 } DiffKind;

typedef struct {
    DiffKind kind;
    char    *text;   /* malloc'd, NUL-terminated, the line's trailing newline stripped */
} DiffLine;

typedef struct {
    DiffLine *lines;
    size_t    count;
    int       added;     /* number of DIFF_ADDED lines */
    int       removed;   /* number of DIFF_REMOVED lines */
} Diff;

Diff forge_diff(const char *a, size_t alen, const char *b, size_t blen);
void forge_diff_free(Diff *d);

#endif /* FORGE_DIFF_H */
