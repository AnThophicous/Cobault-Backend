/*
 * Camada HTTP sobre libcurl. Um CURLSH é compartilhado pelos workers, então
 * conexões TCP/TLS, cache de DNS e tickets TLS são reaproveitados entre eles.
 * Cada worker tem um handle próprio com keep-alive. Corpos de Range vão para
 * o arquivo com pwrite, sem cópia intermediária.
 */
#ifndef CB_HTTP_H
#define CB_HTTP_H

#include <signal.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

/* Bytes baixados no processo inteiro (base da barra de progresso). */
extern _Atomic uint64_t cb_bytes_done;
/* Definido por SIGINT: todas as operações de rede devem parar. */
extern volatile sig_atomic_t cb_interrupted;

enum {
    CB_HTTP_OK = 0,
    CB_HTTP_NET = 1,      /* falha de rede/TLS/timeout (retentável) */
    CB_HTTP_STATUS = 2,   /* HTTP 4xx/5xx ou status inesperado */
    CB_HTTP_ABORT = 3,    /* interrompido pelo usuário */
    CB_HTTP_IO = 4        /* erro ao escrever no arquivo local */
};

typedef struct cb_share cb_share;
typedef struct cb_http cb_http;

typedef struct {
    char *data;           /* sempre terminado em NUL (útil para texto/JSON) */
    size_t len;
    size_t cap;
} cb_buf;

typedef struct {
    long status;
    int64_t content_length;  /* -1 se desconhecido */
    int64_t range_total;     /* total de Content-Range (-1 se ausente) */
} cb_resp;

typedef struct {
    const char *user_agent;     /* NULL = padrão do Cobault */
    const char *range;          /* "a-b" (formato do CURLOPT_RANGE) ou NULL */
    const char *content_type;   /* usado junto com body */
    const char *body;           /* NULL = GET */
    size_t body_len;
    const char *const *headers; /* NULL-terminado, "Nome: valor" */
    int text;                   /* permite gzip/deflate (playlists, JSON) */
    int require_partial;        /* exige 206 antes de gravar (chunks) */
    size_t max_bytes;           /* >0: para de ler após N bytes (sondagem) */
} cb_reqopts;

int cb_http_global_init(void);
void cb_http_global_cleanup(void);

cb_share *cb_share_new(void);
void cb_share_free(cb_share *s);

cb_http *cb_http_new(cb_share *s);
void cb_http_free(cb_http *h);

void cb_buf_free(cb_buf *b);

/* Requisições únicas (sem retry). Retornam CB_HTTP_*. */
int cb_http_get_mem(cb_http *h, const char *url, const cb_reqopts *o,
                    cb_buf *out, cb_resp *resp);
int cb_http_get_fd(cb_http *h, const char *url, const cb_reqopts *o,
                   int fd, off_t pos, cb_resp *resp);

/* Com retentativas: backoff exponencial com jitter, interrompível. */
int cb_http_get_mem_retry(cb_http *h, const char *url, const cb_reqopts *o,
                          cb_buf *out, cb_resp *resp, int retries);
int cb_http_get_fd_retry(cb_http *h, const char *url, const cb_reqopts *o,
                         int fd, off_t pos, cb_resp *resp, int retries);

/* Texto curto descrevendo um resultado (para mensagens de erro). */
void cb_http_describe(int rc, long status, char *buf, size_t buflen);

#endif
