#include "nb_arena.h"
#include <stdlib.h>
#include <string.h>

#define DEFAULT_BLOCK_SIZE (1024 * 64)   /* 64 KB per block */
#define ALIGN_UP(n, a)     (((n) + (a) - 1) & ~((a) - 1))
#define ALIGN_OF           (sizeof(void*))

struct NbArenaBlock {
    NbArenaBlock *next;
    size_t        cap;
    size_t        used;
    char          data[];
};

NbArena *nb_arena_new(size_t block_size) {
    NbArena *a = calloc(1, sizeof(*a));
    if (!a) return NULL;
    a->block_size = block_size ? block_size : DEFAULT_BLOCK_SIZE;
    return a;
}

static NbArenaBlock *new_block(size_t sz) {
    NbArenaBlock *b = malloc(sizeof(NbArenaBlock) + sz);
    if (!b) return NULL;
    b->next = NULL;
    b->cap  = sz;
    b->used = 0;
    return b;
}

void *nb_arena_alloc(NbArena *a, size_t size) {
    size = ALIGN_UP(size, ALIGN_OF);
    if (!size) return NULL;

    /* Try current head block first */
    if (a->head && (a->head->cap - a->head->used) >= size) {
        void *ptr = a->head->data + a->head->used;
        a->head->used += size;
        return ptr;
    }

    /* Allocate a new block */
    size_t bsz = size > a->block_size ? size : a->block_size;
    NbArenaBlock *b = new_block(bsz);
    if (!b) return NULL;
    b->next  = a->head;
    a->head  = b;
    b->used  = size;
    return b->data;
}

void *nb_arena_alloc0(NbArena *a, size_t size) {
    void *p = nb_arena_alloc(a, size);
    if (p) memset(p, 0, size);
    return p;
}

char *nb_arena_strdup(NbArena *a, const char *s) {
    return s ? nb_arena_strndup(a, s, strlen(s)) : NULL;
}

char *nb_arena_strndup(NbArena *a, const char *s, size_t n) {
    if (!s) return NULL;
    char *p = nb_arena_alloc(a, n + 1);
    if (!p) return NULL;
    memcpy(p, s, n);
    p[n] = '\0';
    return p;
}

void nb_arena_reset(NbArena *a) {
    NbArenaBlock *b = a->head;
    while (b) {
        NbArenaBlock *next = b->next;
        free(b);
        b = next;
    }
    a->head = NULL;
}

void nb_arena_free(NbArena *a) {
    if (!a) return;
    nb_arena_reset(a);
    free(a);
}
