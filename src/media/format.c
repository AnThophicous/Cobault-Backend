#include "format.h"

#include "../core/util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum {
    SEL_BEST, SEL_WORST,
    SEL_BESTVIDEO, SEL_WORSTVIDEO,
    SEL_BESTAUDIO, SEL_WORSTAUDIO,
    SEL_ID
} sel_kind;

typedef enum { OP_LT, OP_LE, OP_GT, OP_GE, OP_EQ, OP_NE } cmp_op;

typedef enum { KEY_HEIGHT, KEY_WIDTH, KEY_FPS, KEY_TBR, KEY_FILESIZE, KEY_EXT, KEY_FORMAT_ID } filt_key;

typedef struct {
    filt_key key;
    cmp_op op;
    double num;
    char str[64];
    int is_num;
} filter;

#define MAX_FILTERS 8

typedef struct {
    sel_kind kind;
    int include_muxed;        /* bv*, ba*, wv*, wa* */
    char id[128];             /* para SEL_ID */
    filter filters[MAX_FILTERS];
    int nfilters;
} part;

typedef struct {
    part a;
    int has_b;
    part b;
} alt;

typedef struct {
    alt *alts;
    int count;
} spec_t;

static void set_err(char *err, size_t errlen, const char *msg, const char *near)
{
    if (!err || !errlen)
        return;
    if (near)
        snprintf(err, errlen, "%s perto de \"%.40s\"", msg, near);
    else
        snprintf(err, errlen, "%s", msg);
}

static int parse_op(const char **pp, cmp_op *op)
{
    const char *p = *pp;
    if (p[0] == '<' && p[1] == '=') { *op = OP_LE; *pp = p + 2; return 1; }
    if (p[0] == '>' && p[1] == '=') { *op = OP_GE; *pp = p + 2; return 1; }
    if (p[0] == '!' && p[1] == '=') { *op = OP_NE; *pp = p + 2; return 1; }
    if (p[0] == '<') { *op = OP_LT; *pp = p + 1; return 1; }
    if (p[0] == '>') { *op = OP_GT; *pp = p + 1; return 1; }
    if (p[0] == '=') { *op = OP_EQ; *pp = p + 1; return 1; }
    return 0;
}

static int parse_filter(const char **pp, filter *f, char *err, size_t errlen)
{
    const char *p = *pp + 1; /* pula '[' */
    char key[32];
    size_t kn = 0;
    while (*p && ((*p >= 'a' && *p <= 'z') || *p == '_') && kn < sizeof key - 1)
        key[kn++] = *p++;
    key[kn] = '\0';

    if (!strcmp(key, "height")) f->key = KEY_HEIGHT;
    else if (!strcmp(key, "width")) f->key = KEY_WIDTH;
    else if (!strcmp(key, "fps")) f->key = KEY_FPS;
    else if (!strcmp(key, "tbr")) f->key = KEY_TBR;
    else if (!strcmp(key, "filesize")) f->key = KEY_FILESIZE;
    else if (!strcmp(key, "ext")) f->key = KEY_EXT;
    else if (!strcmp(key, "format_id")) f->key = KEY_FORMAT_ID;
    else {
        set_err(err, errlen, "filtro desconhecido", *pp);
        return -1;
    }

    if (!parse_op(&p, &f->op)) {
        set_err(err, errlen, "operador de filtro inválido", *pp);
        return -1;
    }

    size_t vn = 0;
    char val[64];
    while (*p && *p != ']' && vn < sizeof val - 1)
        val[vn++] = *p++;
    val[vn] = '\0';
    if (*p != ']') {
        set_err(err, errlen, "filtro sem ']'", *pp);
        return -1;
    }
    p++;

    char *endp;
    f->num = strtod(val, &endp);
    f->is_num = (val[0] != '\0' && *endp == '\0');
    snprintf(f->str, sizeof f->str, "%s", val);

    if ((f->key != KEY_EXT && f->key != KEY_FORMAT_ID) && !f->is_num) {
        set_err(err, errlen, "valor numérico esperado no filtro", *pp);
        return -1;
    }
    *pp = p;
    return 0;
}

static int name_to_kind(const char *name, size_t n, sel_kind *kind, int *muxed)
{
    struct { const char *s; sel_kind k; int muxed; } table[] = {
        { "best", SEL_BEST, 0 },           { "b", SEL_BEST, 0 },
        { "worst", SEL_WORST, 0 },         { "w", SEL_WORST, 0 },
        { "bestvideo", SEL_BESTVIDEO, 0 }, { "bv", SEL_BESTVIDEO, 0 },
        { "bv*", SEL_BESTVIDEO, 1 },       { "bestvideo*", SEL_BESTVIDEO, 1 },
        { "worstvideo", SEL_WORSTVIDEO, 0 }, { "wv", SEL_WORSTVIDEO, 0 },
        { "wv*", SEL_WORSTVIDEO, 1 },
        { "bestaudio", SEL_BESTAUDIO, 0 }, { "ba", SEL_BESTAUDIO, 0 },
        { "ba*", SEL_BESTAUDIO, 1 },       { "bestaudio*", SEL_BESTAUDIO, 1 },
        { "worstaudio", SEL_WORSTAUDIO, 0 }, { "wa", SEL_WORSTAUDIO, 0 },
        { "wa*", SEL_WORSTAUDIO, 1 },
    };
    for (size_t i = 0; i < sizeof table / sizeof table[0]; i++) {
        if (strlen(table[i].s) == n && memcmp(table[i].s, name, n) == 0) {
            *kind = table[i].k;
            *muxed = table[i].muxed;
            return 1;
        }
    }
    return 0;
}

static int parse_part(const char **pp, part *out, char *err, size_t errlen)
{
    const char *p = *pp;
    memset(out, 0, sizeof *out);

    const char *start = p;
    while (*p && *p != '[' && *p != '+' && *p != '/')
        p++;
    size_t n = (size_t)(p - start);
    if (n == 0) {
        set_err(err, errlen, "seletor vazio", start);
        return -1;
    }
    if (!name_to_kind(start, n, &out->kind, &out->include_muxed)) {
        if (n >= sizeof out->id) {
            set_err(err, errlen, "format_id longo demais", start);
            return -1;
        }
        out->kind = SEL_ID;
        memcpy(out->id, start, n);
        out->id[n] = '\0';
    }

    while (*p == '[') {
        if (out->nfilters >= MAX_FILTERS) {
            set_err(err, errlen, "filtros demais", p);
            return -1;
        }
        const char *q = p;
        if (parse_filter(&q, &out->filters[out->nfilters], err, errlen) != 0)
            return -1;
        out->nfilters++;
        p = q;
    }

    *pp = p;
    return 0;
}

static int parse_spec(const char *s, spec_t *spec, char *err, size_t errlen)
{
    /* Conta alternativas para alocar com precisão. */
    int n = 1;
    for (const char *c = s; *c; c++)
        if (*c == '/')
            n++;
    spec->alts = calloc((size_t)n, sizeof *spec->alts);
    if (!spec->alts) {
        set_err(err, errlen, "sem memória", NULL);
        return -1;
    }

    const char *p = s;
    for (int i = 0; i < n; i++) {
        alt *al = &spec->alts[i];
        if (parse_part(&p, &al->a, err, errlen) != 0)
            return -1;
        if (*p == '+') {
            p++;
            al->has_b = 1;
            if (parse_part(&p, &al->b, err, errlen) != 0)
                return -1;
        }
        if (*p == '/') {
            p++;
        } else if (*p != '\0') {
            set_err(err, errlen, "caractere inesperado", p);
            return -1;
        }
    }
    spec->count = n;
    return 0;
}

int cb_format_check_syntax(const char *s, char *err, size_t errlen)
{
    spec_t spec = { 0 };
    int rc = parse_spec(s, &spec, err, errlen);
    free(spec.alts);
    return rc;
}

static int cmp_num(double v, cmp_op op, double ref)
{
    switch (op) {
    case OP_LT: return v < ref;
    case OP_LE: return v <= ref;
    case OP_GT: return v > ref;
    case OP_GE: return v >= ref;
    case OP_EQ: return v == ref;
    case OP_NE: return v != ref;
    }
    return 0;
}

static int matches_filter(const cb_format *f, const filter *flt)
{
    switch (flt->key) {
    case KEY_HEIGHT:   return cmp_num(f->height, flt->op, flt->num);
    case KEY_WIDTH:    return cmp_num(f->width, flt->op, flt->num);
    case KEY_FPS:      return cmp_num(f->fps, flt->op, flt->num);
    case KEY_TBR:      return cmp_num(f->tbr, flt->op, flt->num);
    case KEY_FILESIZE: return cmp_num((double)f->filesize, flt->op, flt->num);
    case KEY_EXT: {
        int eq = strcmp(f->ext, flt->str) == 0;
        return flt->op == OP_NE ? !eq : eq;
    }
    case KEY_FORMAT_ID: {
        int eq = strcmp(f->format_id, flt->str) == 0;
        return flt->op == OP_NE ? !eq : eq;
    }
    }
    return 0;
}

/* Ordenação: retorna <0 se a é melhor que b. */
static int rank_cmp(const cb_format *a, const cb_format *b, sel_kind kind, int worst)
{
    double ka, kb;
    int s = worst ? -1 : 1;   /* inverte só os critérios numéricos */
    switch (kind) {
    case SEL_BESTVIDEO:
    case SEL_WORSTVIDEO:
        /* Resolução > fps > bitrate. */
        if (a->height != b->height) return s * (b->height - a->height);
        if (a->fps != b->fps) return s * (b->fps - a->fps);
        ka = a->tbr; kb = b->tbr;
        break;
    case SEL_BESTAUDIO:
    case SEL_WORSTAUDIO:
        ka = a->tbr; kb = b->tbr;
        break;
    default: { /* best / worst combinados: muxado primeiro, depois resolução. */
        int am = a->has_audio && a->has_video;
        int bm = b->has_audio && b->has_video;
        if (am != bm) return bm - am;   /* muxado antes, mesmo em worst */
        if (a->height != b->height) return s * (b->height - a->height);
        ka = a->tbr; kb = b->tbr;
        break;
    }
    }
    if (ka > kb) return -s;
    if (ka < kb) return s;
    return 0;
}

static int candidate_ok(const cb_format *f, const part *p)
{
    if (f->unavailable)
        return 0;
    /* Seleção implícita pula formatos que o servidor não entrega por inteiro
       (ex.: IOS libera só uma faixa inicial). Pedir pelo id exato continua permitido. */
    if (p->kind != SEL_ID && f->max_bytes > 0 && f->filesize > f->max_bytes)
        return 0;

    switch (p->kind) {
    case SEL_BESTVIDEO:
    case SEL_WORSTVIDEO:
        if (!f->has_video) return 0;
        if (!p->include_muxed && f->has_audio) return 0;
        break;
    case SEL_BESTAUDIO:
    case SEL_WORSTAUDIO:
        if (!f->has_audio) return 0;
        if (!p->include_muxed && f->has_video) return 0;
        break;
    case SEL_ID:
        if (strcmp(f->format_id, p->id) != 0) return 0;
        break;
    default:
        break;
    }
    for (int i = 0; i < p->nfilters; i++)
        if (!matches_filter(f, &p->filters[i]))
            return 0;
    return 1;
}

static const cb_format *pick(const cb_media *m, const part *p)
{
    const cb_format *best = NULL;
    int worst = (p->kind == SEL_WORST || p->kind == SEL_WORSTVIDEO || p->kind == SEL_WORSTAUDIO);
    for (size_t i = 0; i < m->count; i++) {
        const cb_format *f = &m->formats[i];
        if (!candidate_ok(f, p))
            continue;
        if (!best) {
            best = f;
            continue;
        }
        int c = rank_cmp(f, best, p->kind, worst);
        if (c < 0)
            best = f;
    }
    return best;
}

int cb_format_select(const cb_media *m, const char *s, cb_selection *sel, char *err, size_t errlen)
{
    spec_t spec = { 0 };
    if (parse_spec(s, &spec, err, errlen) != 0) {
        free(spec.alts);
        return -1;
    }

    int rc = -1;
    memset(sel, 0, sizeof *sel);
    for (int i = 0; i < spec.count && rc != 0; i++) {
        alt *al = &spec.alts[i];
        if (!al->has_b) {
            const cb_format *f = pick(m, &al->a);
            if (f) {
                sel->single = f;
                rc = 0;
            }
            continue;
        }
        const cb_format *va = pick(m, &al->a);
        const cb_format *vb = pick(m, &al->b);
        if (va && vb) {
            /* Merge: a primeira parte deve ser vídeo, a segunda áudio. */
            if (va == vb) {
                sel->single = va;
            } else {
                sel->video = va;
                sel->audio = vb;
            }
            rc = 0;
        }
    }

    if (rc != 0)
        set_err(err, errlen, "nenhum formato corresponde ao seletor", s);
    free(spec.alts);
    return rc;
}

static const char *dash(const char *s) { return (s && s[0]) ? s : "-"; }

void cb_format_print_table(const cb_media *m)
{
    printf("[%s] %s\n", m->extractor, m->title[0] ? m->title : "(sem título)");
    printf("%-10s %-6s %-12s %-5s %-9s %-6s %-22s %-22s %-12s %s\n",
           "ID", "EXT", "RESOLUÇÃO", "FPS", "TBR", "PROTO", "VCODEC", "ACODEC", "TAMANHO", "NOTA");
    for (size_t i = 0; i < m->count; i++) {
        const cb_format *f = &m->formats[i];
        char res[24] = "audio only";
        if (f->has_video) {
            if (f->width && f->height) snprintf(res, sizeof res, "%dx%d", f->width, f->height);
            else if (f->height) snprintf(res, sizeof res, "%dp", f->height);
            else snprintf(res, sizeof res, "video");
        }
        char tbr[16] = "-", size[24] = "-", fps[16] = "-";
        if (f->tbr > 0) snprintf(tbr, sizeof tbr, "%.0fk", f->tbr);
        if (f->fps > 0) snprintf(fps, sizeof fps, "%d", f->fps);
        if (f->filesize > 0) cb_fmt_size((double)f->filesize, size, sizeof size);
        char note[48] = "";
        if (f->max_bytes > 0) {
            char lim[24];
            cb_fmt_size((double)f->max_bytes, lim, sizeof lim);
            snprintf(note, sizeof note, "limite do cliente: %s", lim);
        }
        printf("%-10s %-6s %-12s %-5s %-9s %-6s %-22s %-22s %-12s %s\n",
               f->format_id, dash(f->ext), res, fps, tbr,
               f->proto == CB_PROTO_HLS ? "hls" : "https",
               dash(f->vcodec), dash(f->acodec), size, note);
    }
}
