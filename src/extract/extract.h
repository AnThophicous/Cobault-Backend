/*
 * Extração: descobre os formatos disponíveis para uma URL.
 *
 * Provedores (escolhidos pela URL):
 *   youtube : API interna do player (clientes VISIONOS, IOS e ANDROID)
 *   hls     : playlist m3u8 (master ou mídia)
 *   direct  : arquivo HTTP(S) único (com ou sem Range)
 */
#ifndef CB_EXTRACT_H
#define CB_EXTRACT_H

#include <stddef.h>

#include "../media/media.h"
#include "../net/http.h"

typedef struct {
    cb_http *http;
    const char *yt_client;   /* "auto", "visionos", "ios" ou "android" */
} cb_extract_opts;

cb_media *cb_extract(const cb_extract_opts *o, const char *url, char *err, size_t errlen);

int cb_is_youtube_url(const char *url);
cb_media *cb_youtube_extract(const cb_extract_opts *o, const char *url, char *err, size_t errlen);

/* Adiciona as variantes de um manifesto master HLS ao media. */
int cb_extract_hls_manifest(cb_media *m, cb_http *h, const char *manifest_url,
                            const char *text, char *err, size_t errlen);

#endif
