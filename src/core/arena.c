#include "arena.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define CB_ALIGN 16

struct cb_block {
    struct cb_block *next;
    size_t cap;
    size_t used;
};

/* Cabeçalho arredondado para CB_ALIGN: os dados seguem alinhados. */
#define CB_BLOCK_HDR ((sizeof(struct cb_block) + (CB_ALIGN - 1)) & ~(size_t)(CB_ALIGN - 1))

struct cb_arena {
    struct cb_block *head;
    size_t block_size;
};

cb_arena *cb_arena_new(size_t block_size)
{
    cb_arena *a = calloc(1, sizeof *a);
    if (!a)
        return NULL;
    a->block_size = block_size ? block_size : 64 * 1024;
    return a;
}

void *cb_arena_alloc(cb_arena *a, size_t size)
{
    size = (size + (CB_ALIGN - 1)) & ~(size_t)(CB_ALIGN - 1);
    if (size == 0)
        size = CB_ALIGN;

    struct cb_block *b = a->head;
    if (!b || b->cap - b->used < size) {
        size_t cap = size > a->block_size ? size : a->block_size;
        struct cb_block *nb = malloc(CB_BLOCK_HDR + cap);
        if (!nb)
            return NULL;
        nb->next = a->head;
        nb->cap = cap;
        nb->used = 0;
        a->head = nb;
        b = nb;
    }

    void *p = (unsigned char *)b + CB_BLOCK_HDR + b->used;
    b->used += size;
    return p;
}

char *cb_arena_strndup(cb_arena *a, const char *s, size_t n)
{
    char *d = cb_arena_alloc(a, n + 1);
    if (!d)
        return NULL;
    memcpy(d, s, n);
    d[n] = '\0';
    return d;
}

char *cb_arena_strdup(cb_arena *a, const char *s)
{
    return cb_arena_strndup(a, s, strlen(s));
}

void cb_arena_free(cb_arena *a)
{
    if (!a)
        return;
    struct cb_block *b = a->head;
    while (b) {
        struct cb_block *next = b->next;
        free(b);
        b = next;
    }
    free(a);
}
