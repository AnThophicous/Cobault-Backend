/*
 * Downloader HLS com segmentos em paralelo e gravação em ordem.
 *
 * Modelo: as tarefas são enfileiradas em ordem de índice (FIFO). Cada worker
 * baixa (e, se preciso, decifra AES-128) um segmento em memória. A thread
 * principal grava os segmentos prontos na sequência correta. Um worker só
 * começa o segmento i quando i < próximo_a_gravar + janela, o que limita a
 * memória em uso sem risco de impasse (o segmento que o escritor espera
 * sempre já foi iniciado).
 *
 * Retomada: .part.state guarda (próximo segmento, offset no arquivo). Ao
 * retomar, o arquivo é cortado no offset salvo e o download continua dali.
 */
#include <errno.h>
#include <fcntl.h>
#include <openssl/evp.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "../core/progress.h"
#include "../core/util.h"
#include "../extract/m3u8.h"
#include "../net/pool.h"
#include "download.h"

#define HLS_MAGIC "CBHLS001"

typedef struct {
    char magic[8];
    uint64_t url_hash;
    uint64_t nseg;
    uint64_t next;     /* índice do próximo segmento a gravar */
    uint64_t offset;   /* bytes já gravados no arquivo */
} hls_state_t;

typedef struct {
    const char *uri;
    uint8_t key[16];
} hls_key_t;

typedef struct {
    uint8_t *buf;
    size_t len;
    int state;         /* 0 = pendente, 1 = pronto, 2 = erro */
} slot_t;

typedef struct {
    const cb_dl_opts *o;
    const char *ua;
    cb_m3u8 pl;
    hls_key_t *keys;
    size_t nkeys;
    slot_t *slots;
    size_t next_write;
    size_t window;
    pthread_mutex_t mu;
    pthread_cond_t cv;
    _Atomic int failed;
    char errmsg[256];
} hls_t;

typedef struct {
    hls_t *s;
    size_t idx;
} task_arg_t;

static void set_failure_locked(hls_t *s, const char *msg)
{
    if (!s->failed) {
        snprintf(s->errmsg, sizeof s->errmsg, "%s", msg);
        atomic_store(&s->failed, 1);
    }
}

/* Espera o condvar por no máximo 200 ms: mantém Ctrl+C responsivo. */
static void wait_short(hls_t *s)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_nsec += 200 * 1000000L;
    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000L;
    }
    pthread_cond_timedwait(&s->cv, &s->mu, &ts);
}

static const uint8_t *find_key(const hls_t *s, const char *uri)
{
    for (size_t i = 0; i < s->nkeys; i++)
        if (strcmp(s->keys[i].uri, uri) == 0)
            return s->keys[i].key;
    return NULL;
}

/* IV explícito, ou o número de sequência como inteiro de 128 bits big-endian. */
static int decrypt_segment(const cb_hls_segment *seg, const uint8_t key[16],
                           const uint8_t *in, size_t inlen, uint8_t **out, size_t *outlen)
{
    if (inlen == 0) {
        *out = NULL;
        *outlen = 0;
        return 0;
    }
    if (inlen % 16 != 0)
        return -1;

    uint8_t iv[16];
    if (seg->has_iv) {
        memcpy(iv, seg->iv, 16);
    } else {
        memset(iv, 0, sizeof iv);
        uint64_t sq = (uint64_t)seg->seq;
        for (int k = 0; k < 8; k++)
            iv[15 - k] = (uint8_t)(sq >> (8 * k));
    }

    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    uint8_t *o = malloc(inlen + 16);
    int l1 = 0, l2 = 0;
    int ok = ctx && o &&
             EVP_DecryptInit_ex(ctx, EVP_aes_128_cbc(), NULL, key, iv) == 1 &&
             EVP_DecryptUpdate(ctx, o, &l1, in, (int)inlen) == 1 &&
             EVP_DecryptFinal_ex(ctx, o + l1, &l2) == 1;
    EVP_CIPHER_CTX_free(ctx);
    if (!ok) {
        free(o);
        return -1;
    }
    *out = o;
    *outlen = (size_t)(l1 + l2);
    return 0;
}

static void segment_task(void *arg, cb_http *h)
{
    task_arg_t *ta = arg;
    hls_t *s = ta->s;
    size_t i = ta->idx;

    /* Limite de janela: não sai à frente do escritor. */
    pthread_mutex_lock(&s->mu);
    while (i >= s->next_write + s->window && !s->failed && !cb_interrupted)
        wait_short(s);
    int skip = s->failed || cb_interrupted;
    pthread_mutex_unlock(&s->mu);
    if (skip)
        return;

    cb_hls_segment *seg = &s->pl.segments[i];
    cb_reqopts ro = { .user_agent = s->ua };
    cb_buf b = { 0 };
    cb_resp resp;
    int rc = cb_http_get_mem_retry(h, seg->uri, &ro, &b, &resp, s->o->retries);

    uint8_t *out = NULL;
    size_t outlen = 0;
    char msg[256] = "";
    int ok = rc == CB_HTTP_OK;

    if (!ok) {
        char desc[96];
        cb_http_describe(rc, resp.status, desc, sizeof desc);
        snprintf(msg, sizeof msg, "segmento %zu: %s", i, desc);
    } else if (seg->key_uri) {
        const uint8_t *key = find_key(s, seg->key_uri);
        if (!key || decrypt_segment(seg, key, (const uint8_t *)b.data, b.len, &out, &outlen) != 0) {
            ok = 0;
            snprintf(msg, sizeof msg, "segmento %zu: falha ao decifrar AES-128", i);
        }
        cb_buf_free(&b);
    } else {
        out = (uint8_t *)b.data;     /* assume posse do buffer */
        outlen = b.len;
        b.data = NULL;
    }

    pthread_mutex_lock(&s->mu);
    if (ok) {
        s->slots[i].buf = out;
        s->slots[i].len = outlen;
        s->slots[i].state = 1;
    } else {
        free(out);
        s->slots[i].state = 2;
        set_failure_locked(s, msg[0] ? msg : "falha desconhecida em segmento");
    }
    pthread_cond_broadcast(&s->cv);
    pthread_mutex_unlock(&s->mu);
    cb_buf_free(&b);
}

/* Baixa as chaves AES-128 distintas uma única vez. */
static int fetch_keys(hls_t *s, cb_http *h, char *err, size_t errlen)
{
    s->keys = calloc(s->pl.nseg ? s->pl.nseg : 1, sizeof *s->keys);
    if (!s->keys) {
        snprintf(err, errlen, "sem memória");
        return -1;
    }
    for (size_t i = 0; i < s->pl.nseg; i++) {
        const char *uri = s->pl.segments[i].key_uri;
        if (!uri || find_key(s, uri))
            continue;
        cb_reqopts ro = { .user_agent = s->ua };
        cb_buf b = { 0 };
        cb_resp resp;
        int rc = cb_http_get_mem_retry(h, uri, &ro, &b, &resp, s->o->retries);
        if (rc != CB_HTTP_OK || b.len != 16) {
            cb_buf_free(&b);
            snprintf(err, errlen, "chave AES-128 inválida em %s", uri);
            return -1;
        }
        s->keys[s->nkeys].uri = uri;
        memcpy(s->keys[s->nkeys].key, b.data, 16);
        s->nkeys++;
        cb_buf_free(&b);
    }
    return 0;
}

static int write_all(int fd, const uint8_t *p, size_t n)
{
    while (n > 0) {
        ssize_t w = write(fd, p, n);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

static int load_state(const char *path, uint64_t url_hash, uint64_t nseg, hls_state_t *st)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;
    ssize_t r = read(fd, st, sizeof *st);
    close(fd);
    if (r != (ssize_t)sizeof *st)
        return -1;
    if (memcmp(st->magic, HLS_MAGIC, 8) != 0 || st->url_hash != url_hash || st->nseg != nseg ||
        st->next > nseg)
        return -1;
    return 0;
}

static void free_slots(hls_t *s, size_t n)
{
    for (size_t i = 0; i < n; i++)
        free(s->slots[i].buf);
}

int cb_download_hls(const cb_dl_opts *o, cb_share *share, const cb_format *f,
                    const char *dest, char *err, size_t errlen)
{
    char part[1024], state_path[1100];
    snprintf(part, sizeof part, "%s.part", dest);
    snprintf(state_path, sizeof state_path, "%s.state", part);

    if (cb_mkdirs_for(dest) != 0) {
        snprintf(err, errlen, "não consegui criar o diretório de %s", dest);
        return -1;
    }

    cb_http *h = cb_http_new(share);
    if (!h) {
        snprintf(err, errlen, "sem memória (cliente HTTP)");
        return -1;
    }

    hls_t s;
    memset(&s, 0, sizeof s);
    s.o = o;
    s.ua = f->user_agent;
    pthread_mutex_init(&s.mu, NULL);
    pthread_cond_init(&s.cv, NULL);

    cb_arena *arena = cb_arena_new(256 * 1024);
    int result = -1;
    int fd = -1;
    cb_pool *pool = NULL;
    task_arg_t *args = NULL;
    size_t nseg = 0;

    /* Playlist de mídia, baixada no momento do download. */
    cb_buf text = { 0 };
    cb_reqopts tro = { .user_agent = s.ua, .text = 1 };
    cb_resp tresp;
    int rc = cb_http_get_mem_retry(h, f->url, &tro, &text, &tresp, o->retries);
    if (rc != CB_HTTP_OK || !arena || !text.data) {
        char d[96];
        cb_http_describe(rc, tresp.status, d, sizeof d);
        snprintf(err, errlen, "falha ao baixar a playlist: %s", rc == CB_HTTP_OK ? "resposta vazia" : d);
        goto out;
    }
    if (cb_m3u8_parse(arena, text.data, f->url, &s.pl, err, errlen) != 0)
        goto out;
    if (s.pl.is_master) {
        snprintf(err, errlen, "a URL aponta para uma playlist master; escolha uma variante");
        goto out;
    }
    if (s.pl.nseg == 0) {
        snprintf(err, errlen, "playlist sem segmentos");
        goto out;
    }
    if (!s.pl.ended)
        cb_warn("playlist ao vivo (sem EXT-X-ENDLIST): baixando só os segmentos atuais");

    nseg = s.pl.nseg;
    s.window = (size_t)(o->jobs * 4 > 4 ? o->jobs * 4 : 4);
    s.slots = calloc(nseg, sizeof *s.slots);
    args = calloc(nseg, sizeof *args);
    if (!s.slots || !args) {
        snprintf(err, errlen, "sem memória");
        goto out;
    }

    if (fetch_keys(&s, h, err, errlen) != 0)
        goto out;

    /* Retomada ou começo do zero. */
    uint64_t url_hash = cb_hash64(f->url);
    hls_state_t st;
    size_t start = 0;
    uint64_t offset = 0;
    fd = open(part, O_RDWR | O_CREAT, 0644);
    if (fd < 0) {
        snprintf(err, errlen, "não consegui abrir %s: %s", part, strerror(errno));
        goto out;
    }
    if (load_state(state_path, url_hash, nseg, &st) == 0) {
        struct stat sb;
        if (fstat(fd, &sb) == 0 && (uint64_t)sb.st_size >= st.offset) {
            if (ftruncate(fd, (off_t)st.offset) != 0 || lseek(fd, (off_t)st.offset, SEEK_SET) < 0) {
                snprintf(err, errlen, "não consegui retomar %s", part);
                goto out;
            }
            start = (size_t)st.next;
            offset = st.offset;
            cb_info("retomando no segmento %zu/%zu", start, nseg);
        }
    }
    if (start == 0 && offset == 0) {
        if (ftruncate(fd, 0) != 0 || lseek(fd, 0, SEEK_SET) < 0) {
            snprintf(err, errlen, "não consegui reiniciar %s", part);
            goto out;
        }
        /* Init segment (fMP4) vem antes de qualquer segmento de mídia. */
        if (s.pl.map_uri) {
            cb_reqopts mro = { .user_agent = s.ua };
            cb_buf mb = { 0 };
            cb_resp mresp;
            rc = cb_http_get_mem_retry(h, s.pl.map_uri, &mro, &mb, &mresp, o->retries);
            if (rc != CB_HTTP_OK) {
                char d[96];
                cb_http_describe(rc, mresp.status, d, sizeof d);
                snprintf(err, errlen, "falha ao baixar o init segment: %s", d);
                cb_buf_free(&mb);
                goto out;
            }
            if (write_all(fd, (const uint8_t *)mb.data, mb.len) != 0) {
                snprintf(err, errlen, "erro ao gravar %s: %s", part, strerror(errno));
                cb_buf_free(&mb);
                goto out;
            }
            offset = mb.len;
            cb_buf_free(&mb);
        }
    }

    cb_progress_start(f->format_id, -1);
    pool = cb_pool_new(o->jobs, share);
    if (!pool) {
        cb_progress_stop();
        snprintf(err, errlen, "não consegui criar as threads");
        goto out;
    }
    s.next_write = start;
    for (size_t i = start; i < nseg; i++) {
        args[i].s = &s;
        args[i].idx = i;
        cb_pool_submit(pool, segment_task, &args[i]);
    }

    /* Escritor: grava em ordem à medida que os segmentos ficam prontos. */
    size_t i = start;
    for (; i < nseg; i++) {
        pthread_mutex_lock(&s.mu);
        while (s.slots[i].state == 0 && !s.failed && !cb_interrupted)
            wait_short(&s);
        if (s.slots[i].state != 1 || s.failed || cb_interrupted) {
            pthread_mutex_unlock(&s.mu);
            break;
        }
        uint8_t *buf = s.slots[i].buf;
        size_t len = s.slots[i].len;
        s.slots[i].buf = NULL;
        s.next_write = i + 1;
        pthread_cond_broadcast(&s.cv);
        pthread_mutex_unlock(&s.mu);

        int wrc = write_all(fd, buf, len);
        free(buf);
        if (wrc != 0) {
            pthread_mutex_lock(&s.mu);
            set_failure_locked(&s, "erro ao gravar no disco");
            pthread_cond_broadcast(&s.cv);
            pthread_mutex_unlock(&s.mu);
            break;
        }
        offset += len;

        st = (hls_state_t){ .url_hash = url_hash, .nseg = nseg, .next = i + 1, .offset = offset };
        memcpy(st.magic, HLS_MAGIC, 8);
        if (cb_state_write(state_path, &st, sizeof st) != 0)
            cb_warn("não consegui gravar o estado de retomada");
    }

    /* Para os workers restantes e espera terminarem. */
    pthread_mutex_lock(&s.mu);
    if (i < nseg && !s.failed)
        atomic_store(&s.failed, 1);
    pthread_cond_broadcast(&s.cv);
    pthread_mutex_unlock(&s.mu);
    cb_pool_wait(pool);
    cb_pool_free(pool);
    pool = NULL;
    cb_progress_stop();

    if (cb_interrupted) {
        snprintf(err, errlen, "interrompido; rode de novo para retomar de onde parou");
    } else if (i < nseg) {
        snprintf(err, errlen, "%s (progresso salvo; rode de novo para retomar)",
                 s.errmsg[0] ? s.errmsg : "falha no download");
    } else {
        if (fsync(fd) != 0 || close(fd) != 0) {
            fd = -1;
            snprintf(err, errlen, "erro ao fechar %s", part);
            goto out;
        }
        fd = -1;
        if (rename(part, dest) != 0) {
            snprintf(err, errlen, "não consegui finalizar %s", dest);
            goto out;
        }
        unlink(state_path);
        result = 0;
    }

out:
    if (pool)
        cb_pool_free(pool);
    if (fd >= 0)
        close(fd);
    if (args)
        free(args);
    if (s.slots) {
        free_slots(&s, nseg);
        free(s.slots);
    }
    free(s.keys);
    cb_buf_free(&text);
    if (arena)
        cb_arena_free(arena);
    cb_http_free(h);
    pthread_mutex_destroy(&s.mu);
    pthread_cond_destroy(&s.cv);
    return result;
}
