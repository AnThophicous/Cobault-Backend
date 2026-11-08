/*
 * Download de formatos.
 *
 *   direct : Range paralelo com blocos de chunk_size, escrita com pwrite no
 *            .part (pré-alocado), estado de retomada em bitmap de blocos.
 *   hls    : segmentos baixados em paralelo, gravados em ordem; estado de
 *            retomada = próximo segmento + offset no arquivo.
 *   mux    : junta vídeo e áudio com ffmpeg -c copy (sem recodificar).
 */
#ifndef CB_DOWNLOAD_H
#define CB_DOWNLOAD_H

#include <stddef.h>

#include "../media/media.h"
#include "../net/http.h"

typedef struct {
    int jobs;             /* conexões/threads paralelas */
    size_t chunk_size;    /* bytes por requisição Range (direct) */
    int retries;          /* tentativas extras por requisição */
    const char *ffmpeg;   /* caminho do ffmpeg; NULL = não mesclar */
} cb_dl_opts;

/* Baixa `f` para `dest` (o arquivo só aparece completo ao final). */
int cb_download_format(const cb_dl_opts *o, cb_share *share, const cb_format *f,
                       const char *dest, char *err, size_t errlen);

/* Teste barato de disponibilidade: pede os últimos 16 KiB do arquivo.
   Retorna 1 se o servidor entregou o intervalo, 0 caso contrário. */
int cb_probe_tail(cb_http *h, cb_format *f);

/* Downloader HLS (usado por cb_download_format quando proto == HLS). */
int cb_download_hls(const cb_dl_opts *o, cb_share *share, const cb_format *f,
                    const char *dest, char *err, size_t errlen);

/* Mescla vídeo e áudio em `out` com ffmpeg -c copy. Retorna 0 em sucesso. */
int cb_mux_streams(const char *ffmpeg, const char *video, const char *audio,
                   const char *out, char *err, size_t errlen);

/* Utilitários de estado em disco (escrita atômica: .tmp + rename). */
int cb_state_write(const char *path, const void *data, size_t len);
long cb_state_read(const char *path, void *buf, size_t len);

#endif
