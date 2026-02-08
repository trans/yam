/*
 * yam_arena.c — Bump allocator with block chaining
 *
 * All allocations during a parse go through here.
 * One free at the end. No per-object bookkeeping.
 */

#include "yam/yam.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ── Block ───────────────────────────────────────────────── */

typedef struct yam_block {
    struct yam_block *next;
    size_t            cap;
    size_t            used;
    /* data follows immediately */
} yam_block;

#define BLOCK_DATA(b) ((char *)(b) + sizeof(yam_block))

struct yam_arena {
    yam_block *head;     /* current block (allocates from here) */
    yam_block *blocks;   /* all blocks (for freeing) */
    size_t     default_cap;
};

/* ── Internal ────────────────────────────────────────────── */

static yam_block *block_new(size_t cap) {
    yam_block *b = (yam_block *)malloc(sizeof(yam_block) + cap);
    if (!b) return NULL;
    b->next = NULL;
    b->cap  = cap;
    b->used = 0;
    return b;
}

/* ── Public API ──────────────────────────────────────────── */

yam_arena *yam_arena_new(size_t initial_cap) {
    if (initial_cap < 4096) initial_cap = 4096;

    yam_arena *a = (yam_arena *)malloc(sizeof(yam_arena));
    if (!a) return NULL;

    a->head = block_new(initial_cap);
    if (!a->head) { free(a); return NULL; }

    a->blocks      = a->head;
    a->default_cap = initial_cap;
    return a;
}

void *yam_arena_alloc(yam_arena *a, size_t size, size_t align) {
    yam_block *b = a->head;

    /* align the current position */
    size_t aligned = (b->used + align - 1) & ~(align - 1);

    if (aligned + size > b->cap) {
        /* need a new block — at least double or fit the request */
        size_t cap = a->default_cap;
        if (cap < size + align) cap = size + align;
        if (cap < b->cap * 2) cap = b->cap * 2;

        yam_block *nb = block_new(cap);
        if (!nb) return NULL;

        nb->next  = a->blocks;
        a->blocks = nb;
        a->head   = nb;
        b = nb;
        aligned = 0;
    }

    void *ptr = BLOCK_DATA(b) + aligned;
    b->used = aligned + size;
    return ptr;
}

char *yam_arena_dup(yam_arena *a, const char *src, size_t len) {
    char *dst = (char *)yam_arena_alloc(a, len + 1, 1);
    if (!dst) return NULL;
    memcpy(dst, src, len);
    dst[len] = '\0';
    return dst;
}

/* TODO: retaining the largest block avoids re-allocation when input sizes
 * are stable, but keeps peak memory after a one-time large parse.  Consider
 * adding a cap parameter or a separate yam_arena_shrink() API. */
void yam_arena_reset(yam_arena *a) {
    /* free all blocks except the largest */
    yam_block *b = a->blocks;
    yam_block *keep = NULL;
    size_t max_cap = 0;

    /* find largest block to keep */
    for (yam_block *cur = b; cur; cur = cur->next) {
        if (cur->cap >= max_cap) {
            max_cap = cur->cap;
            keep = cur;
        }
    }

    /* free everything else */
    yam_block *cur = b;
    while (cur) {
        yam_block *next = cur->next;
        if (cur != keep) free(cur);
        cur = next;
    }

    keep->next = NULL;
    keep->used = 0;
    a->head    = keep;
    a->blocks  = keep;
}

/* ── File input ──────────────────────────────────────────── */

yam_str yam_read_file(const char *path, yam_arena *a) {
    FILE *f = fopen(path, "rb");
    if (!f) return (yam_str){NULL, 0};

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (size < 0) { fclose(f); return (yam_str){NULL, 0}; }

    char *buf = yam_arena_alloc(a, (size_t)size, 1);
    if (!buf) { fclose(f); return (yam_str){NULL, 0}; }

    size_t nread = fread(buf, 1, (size_t)size, f);
    fclose(f);

    return (yam_str){buf, nread};
}

void yam_arena_free(yam_arena *a) {
    if (!a) return;
    yam_block *b = a->blocks;
    while (b) {
        yam_block *next = b->next;
        free(b);
        b = next;
    }
    free(a);
}
