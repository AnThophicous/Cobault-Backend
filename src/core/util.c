#include "util.h"

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

int cb_log_level = CB_LOG_INFO;
pthread_mutex_t cb_console_lock = PTHREAD_MUTEX_INITIALIZER;
int cb_console_progress_active = 0;

void cb_logf(int level, const char *fmt, ...)
{
    if (level > cb_log_level)
        return;

    va_list ap;
    va_start(ap, fmt);
    pthread_mutex_lock(&cb_console_lock);
    /* Apaga a linha de progresso antes de imprimir a mensagem. */
    if (cb_console_progress_active)
        fputs("\r\033[K", stderr);
    const char *tag = level == CB_LOG_ERROR ? "erro: "
                    : level == CB_LOG_WARN  ? "aviso: "
                    : level == CB_LOG_DEBUG ? "debug: " : "";
    fputs(tag, stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    fflush(stderr);
    pthread_mutex_unlock(&cb_console_lock);
    va_end(ap);
}

int cb_url_is_http(const char *url)
{
    return cb_str_istarts(url, "http://") || cb_str_istarts(url, "https://");
}

/* Separa "scheme://host" de "/caminho?query" em ponteiros dentro de url. */
static const char *url_path_start(const char *url)
{
    const char *p = strstr(url, "://");
    p = p ? p + 3 : url;
    const char *slash = strchr(p, '/');
    return slash ? slash : url + strlen(url);
}

char *cb_url_resolve(const char *base, const char *rel)
{
    size_t rl = strlen(rel);
    if (cb_url_is_http(rel)) {
        char *o = malloc(rl + 1);
        if (o) memcpy(o, rel, rl + 1);
        return o;
    }

    const char *scheme_end = strstr(base, "://");
    const char *path = url_path_start(base);

    char *out;
    if (rel[0] == '/' && rel[1] == '/') {
        /* //host/caminho: herda só o esquema. */
        size_t sl = scheme_end ? (size_t)(scheme_end - base) + 1 : 0;
        out = malloc(sl + rl + 1);
        if (!out) return NULL;
        memcpy(out, base, sl);
        memcpy(out + sl, rel, rl + 1);
        return out;
    }
    if (rel[0] == '/') {
        out = malloc((size_t)(path - base) + rl + 1);
        if (!out) return NULL;
        memcpy(out, base, (size_t)(path - base));
        memcpy(out + (path - base), rel, rl + 1);
        return out;
    }

    /* Caminho relativo: substitui o último segmento do caminho base. */
    const char *end = path + strcspn(path, "?#");
    const char *last = end;
    while (last > path && last[-1] != '/')
        last--;
    size_t prefix = (size_t)(last - base);
    out = malloc(prefix + rl + 1);
    if (!out) return NULL;
    memcpy(out, base, prefix);
    memcpy(out + prefix, rel, rl + 1);
    return out;
}

void cb_url_basename(const char *url, char *out, size_t outlen)
{
    const char *path = url_path_start(url);
    size_t plen = strcspn(path, "?#");
    const char *end = path + plen;
    const char *p = end;
    while (p > path && p[-1] != '/')
        p--;
    size_t n = (size_t)(end - p);
    if (n >= outlen)
        n = outlen - 1;
    memcpy(out, p, n);
    out[n] = '\0';
}

void cb_url_ext(const char *url, char *out, size_t outlen)
{
    char name[512];
    cb_url_basename(url, name, sizeof name);
    const char *dot = strrchr(name, '.');
    out[0] = '\0';
    if (!dot || dot[1] == '\0') return;
    size_t n = strlen(dot + 1);
    if (n >= outlen) n = outlen - 1;
    for (size_t i = 0; i < n; i++)
        out[i] = (char)tolower((unsigned char)dot[1 + i]);
    out[n] = '\0';
}

int cb_str_istarts(const char *s, const char *prefix)
{
    for (; *prefix; s++, prefix++)
        if (!*s || tolower((unsigned char)*s) != tolower((unsigned char)*prefix))
            return 0;
    return 1;
}

int cb_parse_size(const char *s, int64_t *out)
{
    if (!s || !*s)
        return -1;
    char *end;
    errno = 0;
    double v = strtod(s, &end);
    if (end == s || v < 0 || errno)
        return -1;
    int64_t mult = 1;
    switch (tolower((unsigned char)*end)) {
    case 'k': mult = 1024LL; end++; break;
    case 'm': mult = 1024LL * 1024; end++; break;
    case 'g': mult = 1024LL * 1024 * 1024; end++; break;
    case '\0': break;
    default: return -1;
    }
    if (*end != '\0')
        return -1;
    *out = (int64_t)(v * (double)mult);
    return 0;
}

void cb_fmt_size(double bytes, char *buf, size_t buflen)
{
    static const char *units[] = { "B", "KiB", "MiB", "GiB", "TiB" };
    int u = 0;
    while (bytes >= 1024.0 && u < 4) {
        bytes /= 1024.0;
        u++;
    }
    snprintf(buf, buflen, u == 0 ? "%.0f %s" : "%.1f %s", bytes, units[u]);
}

void cb_sanitize_filename(char *s)
{
    for (char *p = s; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c < 32 || strchr("/\\:*?\"<>|", c))
            *p = '_';
    }
}

/* Modelo de saída: "%(campo)s", com "%%" para "%" literal. */
static const char *tpl_field(const char *name, size_t n, const cb_tpl_fields *f, char *numbuf, size_t numlen)
{
    if (n == 2 && memcmp(name, "id", 2) == 0) return f->id;
    if (n == 5 && memcmp(name, "title", 5) == 0) return f->title;
    if (n == 3 && memcmp(name, "ext", 3) == 0) return f->ext;
    if (n == 9 && memcmp(name, "extractor", 9) == 0) return f->extractor;
    if (n == 9 && memcmp(name, "format_id", 9) == 0) return f->format_id;
    if (n == 6 && memcmp(name, "height", 6) == 0) {
        snprintf(numbuf, numlen, "%lld", (long long)f->height);
        return numbuf;
    }
    return NULL;
}

char *cb_tpl_expand(const char *tpl, const cb_tpl_fields *f)
{
    size_t cap = strlen(tpl) * 2 + 512;
    char *out = malloc(cap);
    if (!out) return NULL;
    size_t n = 0;

    for (const char *p = tpl; *p;) {
        const char *val = NULL;
        char numbuf[32];
        size_t adv = 0;

        if (p[0] == '%' && p[1] == '%') {
            val = "%";
            adv = 2;
        } else if (p[0] == '%' && p[1] == '(') {
            const char *close = strchr(p + 2, ')');
            if (close && close[1] == 's') {
                val = tpl_field(p + 2, (size_t)(close - (p + 2)), f, numbuf, sizeof numbuf);
                if (!val) val = "NA";
                adv = (size_t)(close + 2 - p);
            }
        }

        if (val) {
            /* Só os valores dos campos são sanitizados: o texto do modelo
               (inclusive barras de subpastas) é preservado. */
            char clean[512];
            snprintf(clean, sizeof clean, "%s", val);
            cb_sanitize_filename(clean);
            size_t vl = strlen(clean);
            if (n + vl + 1 >= cap) {
                cap = (n + vl + 1) * 2;
                char *grown = realloc(out, cap);
                if (!grown) { free(out); return NULL; }
                out = grown;
            }
            memcpy(out + n, clean, vl);
            n += vl;
            p += adv;
        } else {
            if (n + 2 >= cap) {
                cap *= 2;
                char *grown = realloc(out, cap);
                if (!grown) { free(out); return NULL; }
                out = grown;
            }
            out[n++] = *p++;
        }
    }
    out[n] = '\0';
    return out;
}

uint64_t cb_hash64(const char *s)
{
    uint64_t h = 1469598103934665603ULL; /* FNV-1a 64 */
    for (; *s; s++) {
        h ^= (unsigned char)*s;
        h *= 1099511628211ULL;
    }
    return h;
}

double cb_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

int cb_mkdirs_for(const char *path)
{
    char *copy = strdup(path);
    if (!copy) return -1;
    for (char *p = copy + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(copy, 0777) != 0 && errno != EEXIST) {
                free(copy);
                return -1;
            }
            *p = '/';
        }
    }
    free(copy);
    return 0;
}

int cb_backoff_ms(int attempt)
{
    static __thread unsigned seed = 0;
    if (!seed)
        seed = (unsigned)time(NULL) ^ (unsigned)(uintptr_t)&seed;
    int base = 500 << (attempt > 5 ? 5 : attempt);
    if (base > 8000)
        base = 8000;
    int jitter = (int)(rand_r(&seed) % (unsigned)(base / 2 + 1));
    return base / 2 + jitter;
}

void cb_sleep_ms(int ms)
{
    struct timespec ts = { .tv_sec = ms / 1000, .tv_nsec = (long)(ms % 1000) * 1000000L };
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR)
        ;
}
