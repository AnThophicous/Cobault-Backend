/*
 * Utilitários: log (coordenado com a barra de progresso), URLs, texto,
 * tamanhos, modelos de nome de arquivo e backoff.
 */
#ifndef CB_UTIL_H
#define CB_UTIL_H

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

enum { CB_LOG_ERROR = 0, CB_LOG_WARN = 1, CB_LOG_INFO = 2, CB_LOG_DEBUG = 3 };

extern int cb_log_level;
extern pthread_mutex_t cb_console_lock;
extern int cb_console_progress_active;

void cb_logf(int level, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

#define cb_error(...) cb_logf(CB_LOG_ERROR, __VA_ARGS__)
#define cb_warn(...)  cb_logf(CB_LOG_WARN, __VA_ARGS__)
#define cb_info(...)  cb_logf(CB_LOG_INFO, __VA_ARGS__)
#define cb_debug(...) cb_logf(CB_LOG_DEBUG, __VA_ARGS__)

int cb_url_is_http(const char *url);
/* Resolve `rel` contra `base` (RFC 3986 simplificado, suficiente para HLS). */
char *cb_url_resolve(const char *base, const char *rel);
/* Nome base do caminho da URL, sem query/fragmento. Ex.: "a/b.mp4" -> "b.mp4". */
void cb_url_basename(const char *url, char *out, size_t outlen);
/* Extensão sem ponto, em minúsculas, do caminho da URL (ou ""). */
void cb_url_ext(const char *url, char *out, size_t outlen);

int cb_str_istarts(const char *s, const char *prefix);
/* Lê "512k", "4M", "1G" ou número puro. Retorna 0 em sucesso. */
int cb_parse_size(const char *s, int64_t *out);
/* Formata tamanho humano em buf ("12.3 MiB"). */
void cb_fmt_size(double bytes, char *buf, size_t buflen);
/* Troca caracteres proibidos em nomes de arquivo por '_' (in place). */
void cb_sanitize_filename(char *s);

/* Campos disponíveis para o modelo de saída. Valores NULL viram "NA". */
typedef struct {
    const char *id;
    const char *title;
    const char *ext;
    const char *extractor;
    const char *format_id;
    int64_t height;
} cb_tpl_fields;

/* Expande o modelo de nome. Retorna string alocada (liberar com free) ou NULL. */
char *cb_tpl_expand(const char *tpl, const cb_tpl_fields *f);

uint64_t cb_hash64(const char *s);
double cb_now(void);
/* Cria diretórios intermediários de `path`. Retorna 0 em sucesso. */
int cb_mkdirs_for(const char *path);
/* Backoff exponencial com jitter, em milissegundos. */
int cb_backoff_ms(int attempt);
void cb_sleep_ms(int ms);

#endif
