#include "m3u8.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../core/util.h"

typedef struct {
    const char *key;
    size_t klen;
    const char *val;
    size_t vlen;
} attr_t;

#define MAX_ATTRS 24

/* Lê "KEY=valor,KEY2="texto, com vírgula",..." */
static int parse_attrs(const char *s, size_t n, attr_t *out)
{
    int count = 0;
    const char *p = s, *end = s + n;
    while (p < end && count < MAX_ATTRS) {
        while (p < end && (*p == ',' || *p == ' ' || *p == '\t'))
            p++;
        if (p >= end)
            break;
        const char *k = p;
        while (p < end && *p != '=' && *p != ',')
            p++;
        if (p >= end || *p != '=')
            break;
        attr_t *a = &out[count++];
        a->key = k;
        a->klen = (size_t)(p - k);
        p++;
        if (p < end && *p == '"') {
            p++;
            a->val = p;
            while (p < end && *p != '"')
                p++;
            a->vlen = (size_t)(p - a->val);
            if (p < end)
                p++;
        } else {
            a->val = p;
            while (p < end && *p != ',')
                p++;
            a->vlen = (size_t)(p - a->val);
        }
    }
    return count;
}

static const attr_t *find_attr(const attr_t *attrs, int n, const char *key)
{
    size_t kl = strlen(key);
    for (int i = 0; i < n; i++)
        if (attrs[i].klen == kl && memcmp(attrs[i].key, key, kl) == 0)
            return &attrs[i];
    return NULL;
}

static char *attr_str(cb_arena *a, const attr_t *at)
{
    return at ? cb_arena_strndup(a, at->val, at->vlen) : NULL;
}

static int hexdigit(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* IV em hexadecimal ("0x...") alinhado à direita em 16 bytes. */
static int parse_iv(const char *v, size_t n, uint8_t iv[16])
{
    if (n >= 2 && (v[0] == '0') && (v[1] == 'x' || v[1] == 'X')) {
        v += 2;
        n -= 2;
    }
    if (n == 0 || n > 32)
        return -1;
    memset(iv, 0, 16);
    /* Lê de trás para frente: último nibble vai para o último byte. */
    for (size_t i = 0; i < n; i++) {
        int h = hexdigit((unsigned char)v[n - 1 - i]);
        if (h < 0)
            return -1;
        size_t pos = 15 - i / 2;
        iv[pos] |= (uint8_t)(i % 2 == 0 ? h : h << 4);
    }
    return 0;
}

/* Dobra a capacidade de um vetor da arena copiando os elementos. */
static void *grow(cb_arena *a, void *old, size_t count, size_t *cap, size_t elem)
{
    if (count < *cap)
        return old;
    size_t ncap = *cap ? *cap * 2 : 16;
    void *n = cb_arena_alloc(a, ncap * elem);
    if (!n)
        return NULL;
    if (count)
        memcpy(n, old, count * elem);
    *cap = ncap;
    return n;
}

int cb_m3u8_is_playlist(const char *data, size_t len)
{
    size_t i = 0;
    if (len >= 3 && (unsigned char)data[0] == 0xEF && (unsigned char)data[1] == 0xBB &&
        (unsigned char)data[2] == 0xBF)
        i = 3;
    while (i < len && (data[i] == ' ' || data[i] == '\t' || data[i] == '\r' || data[i] == '\n'))
        i++;
    return len - i >= 7 && memcmp(data + i, "#EXTM3U", 7) == 0;
}

static char *absolute(cb_arena *a, const char *base, const char *rel, size_t rl)
{
    char *tmp = cb_arena_strndup(a, rel, rl);
    if (!tmp)
        return NULL;
    char *abs = cb_url_resolve(base, tmp);
    if (!abs)
        return NULL;
    char *r = cb_arena_strdup(a, abs);
    free(abs);
    return r;
}

static int tag_is(const char *line, size_t len, const char *tag)
{
    size_t tl = strlen(tag);
    return len >= tl && memcmp(line, tag, tl) == 0;
}

int cb_m3u8_parse(cb_arena *a, const char *text, const char *base_url,
                  cb_m3u8 *out, char *err, size_t errlen)
{
    memset(out, 0, sizeof *out);
    if (!cb_m3u8_is_playlist(text, strlen(text))) {
        snprintf(err, errlen, "conteúdo não é uma playlist m3u8");
        return -1;
    }

    /* Estado corrente das tags que valem para os próximos segmentos. */
    char *cur_key = NULL;
    int cur_has_iv = 0;
    uint8_t cur_iv[16];
    double pending_duration = -1.0;
    cb_hls_variant pending_variant;
    int have_pending_variant = 0;
    size_t seg_cap = 0, var_cap = 0, rend_cap = 0;

    const char *p = text;
    int line_no = 0;
    while (*p) {
        const char *eol = strchr(p, '\n');
        size_t len = eol ? (size_t)(eol - p) : strlen(p);
        const char *line = p;
        p = eol ? eol + 1 : p + len;
        line_no++;

        while (len && (line[len - 1] == '\r' || line[len - 1] == ' ' || line[len - 1] == '\t'))
            len--;
        if (len == 0)
            continue;

        if (line[0] == '#') {
            if (tag_is(line, len, "#EXT-X-STREAM-INF:")) {
                attr_t at[MAX_ATTRS];
                int n = parse_attrs(line + 18, len - 18, at);
                memset(&pending_variant, 0, sizeof pending_variant);
                const attr_t *bw = find_attr(at, n, "BANDWIDTH");
                pending_variant.bandwidth = bw ? strtoll(bw->val, NULL, 10) : 0;
                const attr_t *res = find_attr(at, n, "RESOLUTION");
                if (res)
                    sscanf(res->val, "%dx%d", &pending_variant.width, &pending_variant.height);
                const attr_t *fr = find_attr(at, n, "FRAME-RATE");
                if (fr)
                    pending_variant.fps = strtod(fr->val, NULL);
                pending_variant.codecs = attr_str(a, find_attr(at, n, "CODECS"));
                pending_variant.audio_group = attr_str(a, find_attr(at, n, "AUDIO"));
                have_pending_variant = 1;
                out->is_master = 1;
            } else if (tag_is(line, len, "#EXT-X-MEDIA:")) {
                attr_t at[MAX_ATTRS];
                int n = parse_attrs(line + 13, len - 13, at);
                const attr_t *type = find_attr(at, n, "TYPE");
                if (type && type->vlen == 5 && memcmp(type->val, "AUDIO", 5) == 0) {
                    out->renditions = grow(a, out->renditions, out->nrendition, &rend_cap,
                                           sizeof *out->renditions);
                    cb_hls_rendition *r = &out->renditions[out->nrendition++];
                    memset(r, 0, sizeof *r);
                    r->group = attr_str(a, find_attr(at, n, "GROUP-ID"));
                    r->name = attr_str(a, find_attr(at, n, "NAME"));
                    r->lang = attr_str(a, find_attr(at, n, "LANGUAGE"));
                    const attr_t *uri = find_attr(at, n, "URI");
                    if (uri)
                        r->uri = absolute(a, base_url, uri->val, uri->vlen);
                    const attr_t *def = find_attr(at, n, "DEFAULT");
                    r->is_default = def && def->vlen == 3 && memcmp(def->val, "YES", 3) == 0;
                }
            } else if (tag_is(line, len, "#EXTINF:")) {
                pending_duration = strtod(line + 8, NULL);
            } else if (tag_is(line, len, "#EXT-X-KEY:")) {
                attr_t at[MAX_ATTRS];
                int n = parse_attrs(line + 11, len - 11, at);
                const attr_t *method = find_attr(at, n, "METHOD");
                if (!method || (method->vlen == 4 && memcmp(method->val, "NONE", 4) == 0)) {
                    cur_key = NULL;
                    cur_has_iv = 0;
                } else if (method->vlen == 7 && memcmp(method->val, "AES-128", 7) == 0) {
                    const attr_t *uri = find_attr(at, n, "URI");
                    cur_key = uri ? absolute(a, base_url, uri->val, uri->vlen) : NULL;
                    const attr_t *iv = find_attr(at, n, "IV");
                    cur_has_iv = iv && parse_iv(iv->val, iv->vlen, cur_iv) == 0;
                } else {
                    snprintf(err, errlen, "método de criptografia não suportado: %.*s (linha %d)",
                             (int)method->vlen, method->val, line_no);
                    return -1;
                }
            } else if (tag_is(line, len, "#EXT-X-MAP:")) {
                attr_t at[MAX_ATTRS];
                int n = parse_attrs(line + 11, len - 11, at);
                const attr_t *uri = find_attr(at, n, "URI");
                if (uri)
                    out->map_uri = absolute(a, base_url, uri->val, uri->vlen);
            } else if (tag_is(line, len, "#EXT-X-MEDIA-SEQUENCE:")) {
                out->media_sequence = strtoll(line + 22, NULL, 10);
            } else if (tag_is(line, len, "#EXT-X-ENDLIST")) {
                out->ended = 1;
            }
            continue;
        }

        /* Linha de URI: variante (se veio após STREAM-INF) ou segmento. */
        if (have_pending_variant) {
            pending_variant.uri = absolute(a, base_url, line, len);
            out->variants = grow(a, out->variants, out->nvariant, &var_cap, sizeof *out->variants);
            if (!out->variants || !pending_variant.uri) {
                snprintf(err, errlen, "sem memória");
                return -1;
            }
            out->variants[out->nvariant++] = pending_variant;
            have_pending_variant = 0;
            continue;
        }

        out->segments = grow(a, out->segments, out->nseg, &seg_cap, sizeof *out->segments);
        if (!out->segments) {
            snprintf(err, errlen, "sem memória");
            return -1;
        }
        cb_hls_segment *s = &out->segments[out->nseg++];
        memset(s, 0, sizeof *s);
        s->uri = absolute(a, base_url, line, len);
        s->duration = pending_duration > 0 ? pending_duration : 0.0;
        s->key_uri = cur_key;
        s->has_iv = cur_has_iv;
        if (cur_has_iv)
            memcpy(s->iv, cur_iv, 16);
        pending_duration = -1.0;
    }

    /* Número de sequência implícito: media_sequence + índice do segmento. */
    for (size_t i = 0; i < out->nseg; i++)
        out->segments[i].seq = out->media_sequence + (int64_t)i;

    if (out->is_master && out->nvariant == 0) {
        snprintf(err, errlen, "playlist master sem variantes");
        return -1;
    }
    return 0;
}
