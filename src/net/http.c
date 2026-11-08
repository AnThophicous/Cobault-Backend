#include "http.h"

#include <curl/curl.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../core/util.h"

#define CB_USER_AGENT "Cobault/1.0 (+https://github.com/cobault)"
#define CB_BUFFER_SIZE (256 * 1024)  /* buffer de recepção; o libcurl aceita até 512 KiB */

_Atomic uint64_t cb_bytes_done = 0;
volatile sig_atomic_t cb_interrupted = 0;

struct cb_share {
    CURLSH *sh;
    pthread_mutex_t mu[CURL_LOCK_DATA_LAST];
};

struct cb_http {
    CURL *easy;
    cb_share *share;
};

int cb_http_global_init(void)
{
    return curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK ? 0 : -1;
}

void cb_http_global_cleanup(void)
{
    curl_global_cleanup();
}

/* Locks do CURLSH: um mutex por tipo de dado compartilhado. */
static void share_lock(CURL *h, curl_lock_data data, curl_lock_access access, void *userptr)
{
    (void)h;
    (void)access;
    cb_share *s = userptr;
    pthread_mutex_lock(&s->mu[data]);
}

static void share_unlock(CURL *h, curl_lock_data data, void *userptr)
{
    (void)h;
    cb_share *s = userptr;
    pthread_mutex_unlock(&s->mu[data]);
}

cb_share *cb_share_new(void)
{
    cb_share *s = calloc(1, sizeof *s);
    if (!s)
        return NULL;
    for (int i = 0; i < CURL_LOCK_DATA_LAST; i++)
        pthread_mutex_init(&s->mu[i], NULL);

    s->sh = curl_share_init();
    if (!s->sh) {
        free(s);
        return NULL;
    }
    curl_share_setopt(s->sh, CURLSHOPT_LOCKFUNC, share_lock);
    curl_share_setopt(s->sh, CURLSHOPT_UNLOCKFUNC, share_unlock);
    curl_share_setopt(s->sh, CURLSHOPT_USERDATA, s);
    curl_share_setopt(s->sh, CURLSHOPT_SHARE, CURL_LOCK_DATA_CONNECT);
    curl_share_setopt(s->sh, CURLSHOPT_SHARE, CURL_LOCK_DATA_DNS);
    curl_share_setopt(s->sh, CURLSHOPT_SHARE, CURL_LOCK_DATA_SSL_SESSION);
    curl_share_setopt(s->sh, CURLSHOPT_SHARE, CURL_LOCK_DATA_PSL);
    return s;
}

void cb_share_free(cb_share *s)
{
    if (!s)
        return;
    curl_share_cleanup(s->sh);
    for (int i = 0; i < CURL_LOCK_DATA_LAST; i++)
        pthread_mutex_destroy(&s->mu[i]);
    free(s);
}

cb_http *cb_http_new(cb_share *s)
{
    cb_http *h = calloc(1, sizeof *h);
    if (!h)
        return NULL;
    h->easy = curl_easy_init();
    h->share = s;
    if (!h->easy) {
        free(h);
        return NULL;
    }

    CURL *e = h->easy;
    curl_easy_setopt(e, CURLOPT_SHARE, s->sh);
    curl_easy_setopt(e, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(e, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(e, CURLOPT_MAXREDIRS, 10L);
    curl_easy_setopt(e, CURLOPT_CONNECTTIMEOUT, 15L);
    /* Sem limite total, para não cortar arquivos grandes. Conexão parada
       (menos de 1 byte/s por 30 s) é abortada para o retry entrar cedo. */
    curl_easy_setopt(e, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(e, CURLOPT_LOW_SPEED_TIME, 30L);
    curl_easy_setopt(e, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_2TLS);
    curl_easy_setopt(e, CURLOPT_TCP_KEEPALIVE, 1L);
    curl_easy_setopt(e, CURLOPT_TCP_NODELAY, 1L);
    curl_easy_setopt(e, CURLOPT_BUFFERSIZE, (long)CB_BUFFER_SIZE);
    curl_easy_setopt(e, CURLOPT_USERAGENT, CB_USER_AGENT);
    return h;
}

void cb_http_free(cb_http *h)
{
    if (!h)
        return;
    curl_easy_cleanup(h->easy);
    free(h);
}

void cb_buf_free(cb_buf *b)
{
    free(b->data);
    b->data = NULL;
    b->len = b->cap = 0;
}

static size_t header_cb(char *buf, size_t size, size_t nitems, void *ud)
{
    cb_resp *r = ud;
    size_t n = size * nitems;

    if (cb_str_istarts(buf, "HTTP/")) {
        /* Início de uma resposta (inclui redirecionamentos): zera o estado. */
        r->content_length = -1;
        r->range_total = -1;
    } else if (cb_str_istarts(buf, "content-length:")) {
        r->content_length = strtoll(buf + 15, NULL, 10);
    } else if (cb_str_istarts(buf, "content-range:")) {
        /* Formato "bytes a-b/total"; asterisco no lugar do total = desconhecido. */
        const char *slash = memchr(buf, '/', n);
        if (slash && slash[1] != '*')
            r->range_total = strtoll(slash + 1, NULL, 10);
    }
    return n;
}

struct mem_sink {
    cb_buf *buf;
    uint64_t got;
    size_t max_bytes;   /* 0 = sem limite */
    int truncated;      /* parou de propósito ao atingir max_bytes */
};

static size_t mem_write_cb(char *ptr, size_t size, size_t nmemb, void *ud)
{
    struct mem_sink *s = ud;
    size_t n = size * nmemb;
    if (cb_interrupted)
        return 0;
    if (s->max_bytes && s->buf->len + n > s->max_bytes) {
        /* Corta no limite e marca a parada. O libcurl reporta a recusa de
           escrita como erro de rede; cb_http_get_mem aceita isso se truncated. */
        n = s->max_bytes - s->buf->len;
        s->truncated = 1;
        if (n == 0)
            return 0;
    }

    cb_buf *b = s->buf;
    if (b->len + n + 1 > b->cap) {
        size_t ncap = (b->len + n + 1) * 2;
        if (ncap < 64 * 1024)
            ncap = 64 * 1024;
        char *nd = realloc(b->data, ncap);
        if (!nd)
            return 0;
        b->data = nd;
        b->cap = ncap;
    }
    memcpy(b->data + b->len, ptr, n);
    b->len += n;
    b->data[b->len] = '\0';
    s->got += n;
    atomic_fetch_add(&cb_bytes_done, n);
    return n;
}

struct fd_sink {
    CURL *easy;
    int fd;
    off_t pos;
    uint64_t got;
    int require_partial;
    int checked;
    int rejected;   /* recusou o corpo por status ruim */
    int io_failed; /* erro de escrita local */
};

static size_t fd_write_cb(char *ptr, size_t size, size_t nmemb, void *ud)
{
    struct fd_sink *s = ud;
    size_t n = size * nmemb;
    if (cb_interrupted)
        return 0;

    if (!s->checked) {
        long code = 0;
        curl_easy_getinfo(s->easy, CURLINFO_RESPONSE_CODE, &code);
        s->checked = 1;
        if (code >= 400 || (s->require_partial && code != 206)) {
            s->rejected = 1;
            return 0;
        }
    }

    size_t done = 0;
    while (done < n) {
        ssize_t w = pwrite(s->fd, ptr + done, n - done, s->pos + (off_t)done);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            s->io_failed = 1;
            return 0;
        }
        done += (size_t)w;
    }
    s->pos += (off_t)done;
    s->got += done;
    atomic_fetch_add(&cb_bytes_done, done);
    return n;
}

static int perform(cb_http *h, const char *url, const cb_reqopts *o,
                   size_t (*wfn)(char *, size_t, size_t, void *), void *wud,
                   cb_resp *resp)
{
    CURL *e = h->easy;
    static const cb_reqopts defaults = { 0 };
    if (!o)
        o = &defaults;

    resp->status = 0;
    resp->content_length = -1;
    resp->range_total = -1;

    curl_easy_setopt(e, CURLOPT_URL, url);
    curl_easy_setopt(e, CURLOPT_WRITEFUNCTION, wfn);
    curl_easy_setopt(e, CURLOPT_WRITEDATA, wud);
    curl_easy_setopt(e, CURLOPT_HEADERFUNCTION, header_cb);
    curl_easy_setopt(e, CURLOPT_HEADERDATA, resp);
    curl_easy_setopt(e, CURLOPT_USERAGENT, o->user_agent ? o->user_agent : CB_USER_AGENT);
    curl_easy_setopt(e, CURLOPT_RANGE, o->range);
    curl_easy_setopt(e, CURLOPT_ACCEPT_ENCODING, o->text ? "" : NULL);

    struct curl_slist *hdrs = NULL;
    if (o->headers)
        for (const char *const *hp = o->headers; *hp; hp++)
            hdrs = curl_slist_append(hdrs, *hp);
    if (o->body) {
        curl_easy_setopt(e, CURLOPT_POST, 1L);
        curl_easy_setopt(e, CURLOPT_POSTFIELDSIZE, (long)o->body_len);
        curl_easy_setopt(e, CURLOPT_POSTFIELDS, o->body);
        if (o->content_type) {
            char ct[256];
            snprintf(ct, sizeof ct, "Content-Type: %s", o->content_type);
            hdrs = curl_slist_append(hdrs, ct);
        }
    } else {
        curl_easy_setopt(e, CURLOPT_HTTPGET, 1L);
    }
    curl_easy_setopt(e, CURLOPT_HTTPHEADER, hdrs);

    CURLcode rc = curl_easy_perform(e);

    long code = 0;
    curl_easy_getinfo(e, CURLINFO_RESPONSE_CODE, &code);
    resp->status = code;

    curl_slist_free_all(hdrs);
    curl_easy_setopt(e, CURLOPT_HTTPHEADER, NULL);
    curl_easy_setopt(e, CURLOPT_RANGE, NULL);

    if (cb_interrupted)
        return CB_HTTP_ABORT;
    if (rc != CURLE_OK)
        return CB_HTTP_NET;
    if (code < 200 || code >= 300)
        return CB_HTTP_STATUS;
    return CB_HTTP_OK;
}

int cb_http_get_mem(cb_http *h, const char *url, const cb_reqopts *o, cb_buf *out, cb_resp *resp)
{
    struct mem_sink sink = {
        .buf = out, .got = 0,
        .max_bytes = o ? o->max_bytes : 0, .truncated = 0,
    };
    int rc = perform(h, url, o, mem_write_cb, &sink, resp);
    /* Sondagem com max_bytes: a parada forçada é o resultado esperado. */
    if (sink.truncated && rc == CB_HTTP_NET)
        return CB_HTTP_OK;
    if (rc != CB_HTTP_OK)
        atomic_fetch_sub(&cb_bytes_done, sink.got);
    return rc;
}

int cb_http_get_fd(cb_http *h, const char *url, const cb_reqopts *o, int fd, off_t pos, cb_resp *resp)
{
    struct fd_sink sink = {
        .easy = h->easy, .fd = fd, .pos = pos, .got = 0,
        .require_partial = o && o->require_partial,
    };
    int rc = perform(h, url, o, fd_write_cb, &sink, resp);
    if (rc == CB_HTTP_OK)
        return CB_HTTP_OK;

    /* Bytes já gravados desta tentativa contam como não baixados. */
    atomic_fetch_sub(&cb_bytes_done, sink.got);
    if (sink.io_failed)
        return CB_HTTP_IO;
    if (sink.rejected && rc == CB_HTTP_NET)
        return CB_HTTP_STATUS;
    return rc;
}

static int retriable(int rc, long status)
{
    if (rc == CB_HTTP_NET)
        return 1;
    if (rc == CB_HTTP_STATUS)
        return status == 408 || status == 425 || status == 429 || status >= 500;
    return 0;
}

/* Espera o backoff em fatias curtas para responder a Ctrl+C rapidamente. */
static void wait_backoff(int attempt)
{
    int ms = cb_backoff_ms(attempt);
    while (ms > 0 && !cb_interrupted) {
        int slice = ms > 100 ? 100 : ms;
        cb_sleep_ms(slice);
        ms -= slice;
    }
}

int cb_http_get_mem_retry(cb_http *h, const char *url, const cb_reqopts *o,
                          cb_buf *out, cb_resp *resp, int retries)
{
    for (int attempt = 0;; attempt++) {
        out->len = 0;
        if (out->data)
            out->data[0] = '\0';
        int rc = cb_http_get_mem(h, url, o, out, resp);
        if (rc == CB_HTTP_OK || !retriable(rc, resp->status) || attempt >= retries)
            return rc;
        cb_warn("falha em %s (status %ld), nova tentativa %d/%d",
                url, resp->status, attempt + 1, retries);
        wait_backoff(attempt);
        if (cb_interrupted)
            return CB_HTTP_ABORT;
    }
}

int cb_http_get_fd_retry(cb_http *h, const char *url, const cb_reqopts *o,
                         int fd, off_t pos, cb_resp *resp, int retries)
{
    for (int attempt = 0;; attempt++) {
        int rc = cb_http_get_fd(h, url, o, fd, pos, resp);
        if (rc == CB_HTTP_OK || !retriable(rc, resp->status) || attempt >= retries)
            return rc;
        cb_warn("falha em bloco (status %ld), nova tentativa %d/%d",
                resp->status, attempt + 1, retries);
        wait_backoff(attempt);
        if (cb_interrupted)
            return CB_HTTP_ABORT;
    }
}

void cb_http_describe(int rc, long status, char *buf, size_t buflen)
{
    switch (rc) {
    case CB_HTTP_OK:     snprintf(buf, buflen, "ok"); break;
    case CB_HTTP_NET:    snprintf(buf, buflen, "erro de rede/TLS"); break;
    case CB_HTTP_STATUS: snprintf(buf, buflen, "HTTP %ld", status); break;
    case CB_HTTP_ABORT:  snprintf(buf, buflen, "interrompido"); break;
    case CB_HTTP_IO:     snprintf(buf, buflen, "erro de escrita em disco (%s)", strerror(errno)); break;
    default:             snprintf(buf, buflen, "erro desconhecido"); break;
    }
}
