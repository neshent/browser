#ifndef NB_ARENA_H
#define NB_ARENA_H

#include <stddef.h>

/*
 * Simple bump-pointer arena allocator.
 * Perfect for per-page allocations that all die together (DOM, layout boxes, etc.)
 * Zero-overhead free: just reset the arena when the page is destroyed.
 */

typedef struct NbArenaBlock NbArenaBlock;

typedef struct {
    NbArenaBlock *head;
    size_t        block_size;
} NbArena;

NbArena *nb_arena_new(size_t block_size);
void    *nb_arena_alloc(NbArena *a, size_t size);
void    *nb_arena_alloc0(NbArena *a, size_t size);  /* zero-initialised */
char    *nb_arena_strdup(NbArena *a, const char *s);
char    *nb_arena_strndup(NbArena *a, const char *s, size_t n);
void     nb_arena_reset(NbArena *a);   /* free all but keep arena itself */
void     nb_arena_free(NbArena *a);    /* destroy completely */

#endif /* NB_ARENA_H */
