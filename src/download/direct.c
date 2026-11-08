/*
 * Download HTTP direto em blocos paralelos (Range).
 *
 *   1. Se existe estado compatível (.part.state: mesma URL, total e bloco),
 *      retoma: só baixa os blocos que faltam.
 *   2. Senão, a primeira requisição pede só 1 byte (Range 0-0). Resposta 206
 *      informa o tamanho total e confirma o suporte a Range; resposta 200
 *      significa que o servidor ignorou Range e o corpo inteiro já foi gravado.
 *   3. Todos os blocos vão para o pool; cada um faz pwrite no próprio offset.
 *   4. Depois de cada bloco o bitmap é regravado: Ctrl+C não perde progresso.
 */
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../core/progress.h"
#include "../core/util.h"
#include "../net/pool.h"
#include "download.h"

#define STATE_MAGIC "CBSTAT01"
#define MAX_CHUNKS (1u << 20)

typedef struct {
    char magic[8];
    uint64_t url_hash;
    int64_t total;
    int64_t chunk;
    uint32_t nchunks;
} state_hdr_t;

typedef struct direct direct_t;

typedef struct {
    direct_t *d;
    uint32_t idx;
} chunk_arg_t;

struct direct {
    const cb_dl_opts *o;
    const char *url;
    const char *ua;
    char state_path[1200];
    uint64_t url_hash;
    int fd;
    int64_t total;
    int64_t chunk;
    uint32_t nchunks;
    uint8_t *done;
    pthread_mutex_t mu;
    _Atomic int failed;
    char errmsg[256];
};

static void set_failure(direct_t *d, const char *msg)
{
    pthread_mutex_lock(&d->mu);
    if (!atomic_load(&d->failed)) {
        snprintf(d->errmsg, sizeof d->errmsg, "%s", msg);
        atomic_store(&d->failed, 1);
    }
    pthread_mutex_unlock(&d->mu);
}

static size_t bitmap_bytes(uint32_t nchunks)
{
    return (nchunks + 7u) / 8u;
}

/* Grava cabeçalho + bitmap. Chamar com d->mu travado (ou sem threads). */
static int state_save_locked(direct_t *d)
{
    size_t bm = bitmap_bytes(d->nchunks);
    size_t len = sizeof(state_hdr_t) + bm;
    unsigned char *buf = calloc(1, len);
    if (!buf)
        return -1;

    state_hdr_t h;
    memset(&h, 0, sizeof h);
    memcpy(h.magic, STATE_MAGIC, 8);
    h.url_hash = d->url_hash;
    h.total = d->total;
    h.chunk = d->chunk;
    h.nchunks = d->nchunks;
    memcpy(buf, &h, sizeof h);

    for (uint32_t i = 0; i < d->nchunks; i++)
        if (d->done[i])
            buf[sizeof h + i / 8] |= (unsigned char)(1u << (i % 8));

    int rc = cb_state_write(d->state_path, buf, len);
    free(buf);
    return rc;
}

/* Tenta retomar: valida cabeçalho e carrega o bitmap. Retorna 0 se válido. */
static int state_try_resume(direct_t *d)
{
    state_hdr_t h;
    if (cb_state_read(d->state_path, &h, sizeof h) != (long)sizeof h)
        return -1;
    if (memcmp(h.magic, STATE_MAGIC, 8) != 0 || h.url_hash != d->url_hash ||
        h.chunk != d->chunk || h.total <= 0 || h.nchunks == 0 || h.nchunks > MAX_CHUNKS)
        return -1;

    size_t bm = bitmap_bytes(h.nchunks);
    size_t len = sizeof h + bm;
    unsigned char *buf = malloc(len);
    if (!buf)
        return -1;
    if (cb_state_read(d->state_path, buf, len) != (long)len) {
        free(buf);
        return -1;
    }

    d->total = h.total;
    d->nchunks = h.nchunks;
    d->done = calloc(h.nchunks, 1);
    if (!d->done) {
        free(buf);
        return -1;
    }
    for (uint32_t i = 0; i < d->nchunks; i++)
        d->done[i] = (buf[sizeof h + i / 8] >> (i % 8)) & 1u;
    free(buf);
    return 0;
}

static int64_t chunk_bytes(const direct_t *d, uint32_t idx)
{
    int64_t start = (int64_t)idx * d->chunk;
    int64_t end = start + d->chunk;
    if (end > d->total)
        end = d->total;
    return end - start;
}

static void chunk_task(void *arg, cb_http *h)
{
    chunk_arg_t *ca = arg;
    direct_t *d = ca->d;
    if (atomic_load(&d->failed) || cb_interrupted)
        return;

    int64_t start = (int64_t)ca->idx * d->chunk;
    int64_t end = start + chunk_bytes(d, ca->idx) - 1;
    char range[64];
    snprintf(range, sizeof range, "%" PRId64 "-%" PRId64, start, end);

    cb_reqopts ro = {
        .user_agent = d->ua,
        .range = range,
        .require_partial = 1,
    };
    cb_resp resp;
    int rc = cb_http_get_fd_retry(h, d->url, &ro, d->fd, (off_t)start, &resp, d->o->retries);
    if (rc != CB_HTTP_OK) {
        if (rc != CB_HTTP_ABORT) {
            char desc[96], msg[320];
            cb_http_describe(rc, resp.status, desc, sizeof desc);
            snprintf(msg, sizeof msg, "bloco %u (bytes %s): %s", ca->idx, range, desc);
            if (resp.status == 403)
                snprintf(msg + strlen(msg), sizeof msg - strlen(msg),
                         " (o servidor recusou este intervalo: limite do cliente ou URL expirada;"
                         " tente outro formato com -F ou outro --client)");
            set_failure(d, msg);
        }
        return;
    }

    pthread_mutex_lock(&d->mu);
    d->done[ca->idx] = 1;
    if (state_save_locked(d) != 0)
        cb_warn("não consegui gravar o estado de retomada");
    pthread_mutex_unlock(&d->mu);
}

/* Promove .part para o destino final e apaga o estado. */
static int finalize(const char *part, const char *dest, const char *state)
{
    if (rename(part, dest) != 0) {
        cb_error("não consegui mover %s para %s: %s", part, dest, strerror(errno));
        return -1;
    }
    unlink(state);
    return 0;
}

/* Baixa os blocos pendentes em paralelo. Retorna 0 se todos ficaram prontos. */
static int run_pending(direct_t *d, cb_share *share, const char *label,
                       char *err, size_t errlen)
{
    int64_t already = 0;
    for (uint32_t i = 0; i < d->nchunks; i++)
        if (d->done[i])
            already += chunk_bytes(d, i);

    chunk_arg_t *args = calloc(d->nchunks, sizeof *args);
    cb_pool *pool = cb_pool_new(d->o->jobs, share);
    if (!args || !pool) {
        free(args);
        if (pool)
            cb_pool_free(pool);
        snprintf(err, errlen, "sem memória ou threads");
        return -1;
    }

    cb_progress_start(label, d->total);
    atomic_fetch_add(&cb_bytes_done, (uint64_t)already);

    for (uint32_t i = 0; i < d->nchunks; i++) {
        if (d->done[i])
            continue;
        args[i].d = d;
        args[i].idx = i;
        cb_pool_submit(pool, chunk_task, &args[i]);
    }
    cb_pool_wait(pool);
    cb_pool_free(pool);
    cb_progress_stop();

    int rc = -1;
    if (cb_interrupted) {
        snprintf(err, errlen, "interrompido; rode de novo para retomar de onde parou");
    } else if (atomic_load(&d->failed)) {
        snprintf(err, errlen, "%s (progresso salvo; rode de novo para retomar)", d->errmsg);
    } else {
        rc = 0;
        for (uint32_t i = 0; i < d->nchunks; i++)
            if (!d->done[i]) {
                snprintf(err, errlen, "blocos incompletos");
                rc = -1;
                break;
            }
    }
    free(args);
    return rc;
}

int cb_probe_tail(cb_http *h, cb_format *f)
{
    if (f->verified)
        return !f->unavailable;
    f->verified = 1;
    if (f->proto != CB_PROTO_HTTP || f->filesize <= 0)
        return 1;   /* sem tamanho conhecido não há como testar a cauda */

    int64_t last = f->filesize;
    int64_t start = last > 16384 ? last - 16384 : 0;
    char range[64];
    snprintf(range, sizeof range, "%" PRId64 "-%" PRId64, start, last - 1);

    cb_reqopts ro = { .user_agent = f->user_agent, .range = range };
    cb_buf b = { 0 };
    cb_resp resp;
    int rc = cb_http_get_mem(h, f->url, &ro, &b, &resp);
    cb_buf_free(&b);
    if (rc != CB_HTTP_OK)
        f->unavailable = 1;
    return rc == CB_HTTP_OK;
}

int cb_download_format(const cb_dl_opts *o, cb_share *share, const cb_format *f,
                       const char *dest, char *err, size_t errlen)
{
    if (f->proto == CB_PROTO_HLS)
        return cb_download_hls(o, share, f, dest, err, errlen);

    char part[1024];
    snprintf(part, sizeof part, "%s.part", dest);

    direct_t d;
    memset(&d, 0, sizeof d);
    d.o = o;
    d.url = f->url;
    d.ua = f->user_agent;
    d.url_hash = cb_hash64(f->url);
    d.chunk = (int64_t)o->chunk_size;
    d.fd = -1;
    snprintf(d.state_path, sizeof d.state_path, "%s.state", part);
    pthread_mutex_init(&d.mu, NULL);

    /* Rótulo da barra: nome do arquivo de destino, com ou sem pasta. */
    const char *slash = strrchr(dest, '/');
    char label[256];
    snprintf(label, sizeof label, "%s", slash ? slash + 1 : dest);

    int result = -1;
    cb_http *h = NULL;

    if (cb_mkdirs_for(dest) != 0) {
        snprintf(err, errlen, "não consegui criar o diretório de %s", dest);
        goto out;
    }
    h = cb_http_new(share);
    if (!h) {
        snprintf(err, errlen, "sem memória (cliente HTTP)");
        goto out;
    }

    int resumed = state_try_resume(&d) == 0;
    d.fd = open(part, O_RDWR | O_CREAT, 0644);
    if (d.fd < 0) {
        snprintf(err, errlen, "não consegui abrir %s: %s", part, strerror(errno));
        goto out;
    }
    if (resumed) {
        /* Estado sem arquivo (ou arquivo cortado) não serve: blocos "prontos"
           estariam zerados. Nesse caso recomeça do zero. */
        struct stat st;
        if (fstat(d.fd, &st) != 0 || st.st_size != (off_t)d.total) {
            cb_warn("estado de retomada não bate com %s; recomeçando", part);
            resumed = 0;
            free(d.done);
            d.done = NULL;
        }
    }

    if (!resumed) {
        /* Recomeça do zero. A sonda pede só 1 byte para descobrir o tamanho:
           o bloco 0 entra no pool como os demais, sem atrasar o paralelismo. */
        if (ftruncate(d.fd, 0) != 0) {
            snprintf(err, errlen, "não consegui reiniciar %s: %s", part, strerror(errno));
            goto out;
        }
        const char *range0 = "0-0";
        cb_reqopts ro = { .user_agent = d.ua, .range = range0 };
        cb_resp resp;
        int rc = cb_http_get_fd_retry(h, d.url, &ro, d.fd, 0, &resp, o->retries);
        if (rc != CB_HTTP_OK) {
            char desc[96];
            cb_http_describe(rc, resp.status, desc, sizeof desc);
            snprintf(err, errlen, "falha ao baixar %s: %s", label, desc);
            goto out;
        }

        if (resp.status != 206) {
            /* 200: sem suporte a Range; o corpo completo já está no arquivo. */
            if (fsync(d.fd) != 0 || close(d.fd) != 0) {
                d.fd = -1;
                snprintf(err, errlen, "erro ao fechar %s", part);
                goto out;
            }
            d.fd = -1;
            result = finalize(part, dest, d.state_path) == 0 ? 0 : -1;
            if (result != 0)
                snprintf(err, errlen, "não consegui finalizar %s", dest);
            goto out;
        }

        if (resp.range_total <= 0 || resp.range_total > INT64_MAX / 2) {
            snprintf(err, errlen, "servidor não informou o tamanho do arquivo");
            goto out;
        }
        d.total = resp.range_total;
        d.nchunks = (uint32_t)((d.total + d.chunk - 1) / d.chunk);
        if (d.nchunks > MAX_CHUNKS) {
            snprintf(err, errlen, "arquivo grande demais para o tamanho de bloco (-s)");
            goto out;
        }
        d.done = calloc(d.nchunks, 1);
        if (!d.done) {
            snprintf(err, errlen, "sem memória");
            goto out;
        }
        if (ftruncate(d.fd, (off_t)d.total) != 0) {
            snprintf(err, errlen, "não consegui pré-alocar %s: %s", part, strerror(errno));
            goto out;
        }
        if (state_save_locked(&d) != 0)
            cb_warn("não consegui gravar o estado de retomada");
    }

    if (resumed) {
        uint32_t ready = 0;
        for (uint32_t i = 0; i < d.nchunks; i++)
            ready += d.done[i] ? 1u : 0u;
        cb_info("retomando %s: %u de %u blocos já estavam prontos", label, ready, d.nchunks);
    }

    if (run_pending(&d, share, label, err, errlen) != 0)
        goto out;

    if (fsync(d.fd) != 0 || close(d.fd) != 0) {
        d.fd = -1;
        snprintf(err, errlen, "erro ao fechar %s", part);
        goto out;
    }
    d.fd = -1;
    if (finalize(part, dest, d.state_path) != 0) {
        snprintf(err, errlen, "não consegui finalizar %s", dest);
        goto out;
    }
    result = 0;

out:
    if (d.fd >= 0)
        close(d.fd);
    free(d.done);
    if (h)
        cb_http_free(h);
    pthread_mutex_destroy(&d.mu);
    return result;
}
