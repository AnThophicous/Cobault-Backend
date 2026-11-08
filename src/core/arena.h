/*
 * Alocador por blocos. Metadados de mídia, nós JSON e listas de playlist
 * vivem numa arena e são liberados juntos com cb_arena_free().
 * A memória devolvida não é zerada.
 */
#ifndef CB_ARENA_H
#define CB_ARENA_H

#include <stddef.h>

typedef struct cb_arena cb_arena;

cb_arena *cb_arena_new(size_t block_size);
void     *cb_arena_alloc(cb_arena *a, size_t size);
char     *cb_arena_strndup(cb_arena *a, const char *s, size_t n);
char     *cb_arena_strdup(cb_arena *a, const char *s);
void      cb_arena_free(cb_arena *a);

#endif
