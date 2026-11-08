#include "media.h"

#include <stdlib.h>
#include <string.h>

cb_media *cb_media_new(const char *extractor)
{
    cb_arena *a = cb_arena_new(32 * 1024);
    if (!a)
        return NULL;
    cb_media *m = cb_arena_alloc(a, sizeof *m);
    if (!m) {
        cb_arena_free(a);
        return NULL;
    }
    memset(m, 0, sizeof *m);
    m->arena = a;
    m->extractor = cb_arena_strdup(a, extractor ? extractor : "generic");
    m->id = cb_arena_strdup(a, "");
    m->title = cb_arena_strdup(a, "");
    m->webpage_url = cb_arena_strdup(a, "");
    return m;
}

cb_format *cb_media_add_format(cb_media *m)
{
    if (m->count == m->cap) {
        size_t ncap = m->cap ? m->cap * 2 : 8;
        cb_format *nf = cb_arena_alloc(m->arena, ncap * sizeof *nf);
        if (!nf)
            return NULL;
        if (m->count)
            memcpy(nf, m->formats, m->count * sizeof *nf);
        m->formats = nf;
        m->cap = ncap;
    }
    cb_format *f = &m->formats[m->count++];
    memset(f, 0, sizeof *f);
    f->format_id = cb_arena_strdup(m->arena, "");
    f->url = cb_arena_strdup(m->arena, "");
    f->ext = cb_arena_strdup(m->arena, "");
    return f;
}

void cb_media_free(cb_media *m)
{
    if (!m)
        return;
    cb_arena_free(m->arena);
}
