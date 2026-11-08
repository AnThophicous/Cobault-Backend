#include "json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CB_JSON_MAX_DEPTH 256

typedef struct {
    const char *p;
    const char *end;
    cb_arena *a;
    char *err;
    size_t errlen;
    int failed;
    int depth;
} parser_t;

static int fail(parser_t *ps, const char *msg)
{
    if (!ps->failed && ps->err && ps->errlen)
        snprintf(ps->err, ps->errlen, "JSON inválido: %s", msg);
    ps->failed = 1;
    return 0;
}

static void skip_ws(parser_t *ps)
{
    while (ps->p < ps->end &&
           (*ps->p == ' ' || *ps->p == '\t' || *ps->p == '\n' || *ps->p == '\r'))
        ps->p++;
}

static int hexval(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Lê exatamente 4 dígitos hex a partir de p. */
static int read_hex4(const char *p, const char *end, unsigned *out)
{
    if (end - p < 4)
        return 0;
    unsigned v = 0;
    for (int i = 0; i < 4; i++) {
        int h = hexval((unsigned char)p[i]);
        if (h < 0)
            return 0;
        v = (v << 4) | (unsigned)h;
    }
    *out = v;
    return 1;
}

static size_t put_utf8(unsigned cp, char *o)
{
    if (cp < 0x80) {
        o[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        o[0] = (char)(0xC0 | (cp >> 6));
        o[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        o[0] = (char)(0xE0 | (cp >> 12));
        o[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        o[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    o[0] = (char)(0xF0 | (cp >> 18));
    o[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    o[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    o[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

/* Parseia string JSON; ps->p aponta para a aspa de abertura. */
static char *parse_string(parser_t *ps)
{
    ps->p++;
    const char *start = ps->p;

    /* Saída nunca excede a entrada (escapes encolhem), então dimensiona
       pela distância até a aspa de fechamento. */
    const char *scan = start;
    while (scan < ps->end && *scan != '"') {
        if (*scan == '\\')
            scan++;
        scan++;
    }
    if (scan >= ps->end) {
        fail(ps, "string sem fechamento");
        return NULL;
    }

    size_t cap = (size_t)(scan - start) + 1;
    char *out = cb_arena_alloc(ps->a, cap);
    if (!out) {
        fail(ps, "sem memória");
        return NULL;
    }

    size_t n = 0;
    while (ps->p < ps->end && *ps->p != '"') {
        unsigned char c = (unsigned char)*ps->p;
        if (c == '\\') {
            ps->p++;
            if (ps->p >= ps->end) break;
            switch (*ps->p) {
            case '"':  out[n++] = '"'; ps->p++; break;
            case '\\': out[n++] = '\\'; ps->p++; break;
            case '/':  out[n++] = '/'; ps->p++; break;
            case 'b':  out[n++] = '\b'; ps->p++; break;
            case 'f':  out[n++] = '\f'; ps->p++; break;
            case 'n':  out[n++] = '\n'; ps->p++; break;
            case 'r':  out[n++] = '\r'; ps->p++; break;
            case 't':  out[n++] = '\t'; ps->p++; break;
            case 'u': {
                unsigned cp;
                ps->p++;
                if (!read_hex4(ps->p, ps->end, &cp)) {
                    fail(ps, "escape \\u inválido");
                    return NULL;
                }
                ps->p += 4;
                /* Par substituto: \uD800-\uDBFF seguido de \uDC00-\uDFFF. */
                if (cp >= 0xD800 && cp <= 0xDBFF && ps->end - ps->p >= 6 &&
                    ps->p[0] == '\\' && ps->p[1] == 'u') {
                    unsigned lo;
                    if (read_hex4(ps->p + 2, ps->end, &lo) && lo >= 0xDC00 && lo <= 0xDFFF) {
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        ps->p += 6;
                    }
                }
                if (cp >= 0xD800 && cp <= 0xDFFF)
                    cp = 0xFFFD; /* substituto isolado */
                n += put_utf8(cp, out + n);
                break;
            }
            default:
                fail(ps, "escape desconhecido");
                return NULL;
            }
        } else {
            out[n++] = (char)c;
            ps->p++;
        }
    }
    if (ps->p >= ps->end) {
        fail(ps, "string sem fechamento");
        return NULL;
    }
    ps->p++;
    out[n] = '\0';
    return out;
}

static int parse_number(parser_t *ps, double *out)
{
    char buf[64];
    size_t n = 0;
    while (ps->p < ps->end && n < sizeof buf - 1) {
        char c = *ps->p;
        if ((c >= '0' && c <= '9') || c == '-' || c == '+' || c == '.' || c == 'e' || c == 'E')
            buf[n++] = c, ps->p++;
        else
            break;
    }
    buf[n] = '\0';
    if (n == 0)
        return fail(ps, "número esperado");
    char *endp;
    *out = strtod(buf, &endp);
    if (endp == buf)
        return fail(ps, "número inválido");
    return 1;
}

static int match_literal(parser_t *ps, const char *lit)
{
    size_t n = strlen(lit);
    if ((size_t)(ps->end - ps->p) < n || memcmp(ps->p, lit, n) != 0)
        return 0;
    ps->p += n;
    return 1;
}

static cb_json *node_new(parser_t *ps, cb_json_type t)
{
    cb_json *v = cb_arena_alloc(ps->a, sizeof *v);
    if (!v) {
        fail(ps, "sem memória");
        return NULL;
    }
    /* A arena não zera a memória. Sem isto, first, last, next e count
       podem vir com lixo. */
    memset(v, 0, sizeof *v);
    v->type = t;
    return v;
}

static void attach(cb_json *parent, cb_json *child)
{
    if (parent->last)
        parent->last->next = child;
    else
        parent->first = child;
    parent->last = child;
    parent->count++;
}

static cb_json *parse_value(parser_t *ps)
{
    skip_ws(ps);
    if (ps->p >= ps->end) {
        fail(ps, "fim inesperado");
        return NULL;
    }
    if (++ps->depth > CB_JSON_MAX_DEPTH) {
        fail(ps, "aninhamento excessivo");
        return NULL;
    }

    cb_json *v = NULL;
    char c = *ps->p;

    if (c == '{') {
        v = node_new(ps, CB_JSON_OBJECT);
        if (!v) goto out;
        ps->p++;
        skip_ws(ps);
        if (ps->p < ps->end && *ps->p == '}') {
            ps->p++;
            goto out;
        }
        for (;;) {
            skip_ws(ps);
            if (ps->p >= ps->end || *ps->p != '"') {
                fail(ps, "chave esperada");
                v = NULL;
                goto out;
            }
            char *key = parse_string(ps);
            if (!key) { v = NULL; goto out; }
            skip_ws(ps);
            if (ps->p >= ps->end || *ps->p != ':') {
                fail(ps, "':' esperado");
                v = NULL;
                goto out;
            }
            ps->p++;
            cb_json *member = parse_value(ps);
            if (!member) { v = NULL; goto out; }
            member->key = key;
            attach(v, member);
            skip_ws(ps);
            if (ps->p < ps->end && *ps->p == ',') {
                ps->p++;
                continue;
            }
            if (ps->p < ps->end && *ps->p == '}') {
                ps->p++;
                break;
            }
            fail(ps, "',' ou '}' esperado");
            v = NULL;
            goto out;
        }
    } else if (c == '[') {
        v = node_new(ps, CB_JSON_ARRAY);
        if (!v) goto out;
        ps->p++;
        skip_ws(ps);
        if (ps->p < ps->end && *ps->p == ']') {
            ps->p++;
            goto out;
        }
        for (;;) {
            cb_json *item = parse_value(ps);
            if (!item) { v = NULL; goto out; }
            attach(v, item);
            skip_ws(ps);
            if (ps->p < ps->end && *ps->p == ',') {
                ps->p++;
                continue;
            }
            if (ps->p < ps->end && *ps->p == ']') {
                ps->p++;
                break;
            }
            fail(ps, "',' ou ']' esperado");
            v = NULL;
            goto out;
        }
    } else if (c == '"') {
        v = node_new(ps, CB_JSON_STRING);
        if (!v) goto out;
        v->string = parse_string(ps);
        if (!v->string) v = NULL;
    } else if (match_literal(ps, "true")) {
        v = node_new(ps, CB_JSON_BOOL);
        if (v) v->boolean = 1;
    } else if (match_literal(ps, "false")) {
        v = node_new(ps, CB_JSON_BOOL);
    } else if (match_literal(ps, "null")) {
        v = node_new(ps, CB_JSON_NULL);
    } else if (c == '-' || (c >= '0' && c <= '9')) {
        v = node_new(ps, CB_JSON_NUMBER);
        if (v && !parse_number(ps, &v->number)) v = NULL;
    } else {
        fail(ps, "valor inesperado");
    }

out:
    ps->depth--;
    return v;
}

cb_json *cb_json_parse(cb_arena *a, const char *src, size_t len, char *err, size_t errlen)
{
    parser_t ps = {
        .p = src, .end = src + len, .a = a,
        .err = err, .errlen = errlen, .failed = 0, .depth = 0,
    };
    if (err && errlen)
        err[0] = '\0';

    cb_json *root = parse_value(&ps);
    if (!root || ps.failed)
        return NULL;

    skip_ws(&ps);
    if (ps.p != ps.end) {
        fail(&ps, "conteúdo após o valor");
        return NULL;
    }
    return root;
}

const cb_json *cb_json_get(const cb_json *obj, const char *key)
{
    if (!obj || obj->type != CB_JSON_OBJECT)
        return NULL;
    for (const cb_json *m = obj->first; m; m = m->next)
        if (m->key && strcmp(m->key, key) == 0)
            return m;
    return NULL;
}

const char *cb_json_str(const cb_json *v)
{
    return (v && v->type == CB_JSON_STRING) ? v->string : NULL;
}

int64_t cb_json_i64(const cb_json *v, int64_t fallback)
{
    if (!v)
        return fallback;
    if (v->type == CB_JSON_NUMBER)
        return (int64_t)v->number;
    if (v->type == CB_JSON_STRING && v->string && v->string[0]) {
        char *endp;
        long long x = strtoll(v->string, &endp, 10);
        if (endp != v->string)
            return (int64_t)x;
    }
    return fallback;
}

double cb_json_num(const cb_json *v, double fallback)
{
    return (v && v->type == CB_JSON_NUMBER) ? v->number : fallback;
}
