/*
 * Modelo de mídia extraída: formatos, metadados e dono da arena.
 */
#ifndef CB_MEDIA_H
#define CB_MEDIA_H

#include <stddef.h>
#include <stdint.h>

#include "../core/arena.h"

typedef enum {
    CB_PROTO_HTTP = 0,   /* arquivo único (suporta Range) */
    CB_PROTO_HLS = 1     /* playlist m3u8 com segmentos */
} cb_proto;

typedef struct {
    char *format_id;     /* itag do YouTube, id da variante HLS, "direct"... */
    char *url;
    char *ext;           /* mp4, webm, m4a, ts, ... */
    char *vcodec;        /* NULL = desconhecido; "none" = sem vídeo */
    char *acodec;        /* NULL = desconhecido; "none" = sem áudio */
    char *user_agent;    /* NULL = padrão do Cobault */
    int width;
    int height;
    int fps;
    double tbr;          /* taxa total em kbit/s (0 = desconhecida) */
    int64_t filesize;    /* bytes (0 = desconhecido) */
    int64_t max_bytes;   /* bytes que o servidor libera (0 = sem limite conhecido) */
    int verified;        /* teste de cauda já feito */
    int unavailable;     /* teste de cauda falhou: o servidor não entrega o arquivo */
    cb_proto proto;
    int has_video;
    int has_audio;
} cb_format;

typedef struct {
    cb_arena *arena;     /* todas as strings e formatos deste item */
    char *id;
    char *title;
    char *extractor;
    char *webpage_url;
    cb_format *formats;
    size_t count;
    size_t cap;
} cb_media;

cb_media *cb_media_new(const char *extractor);
/* Reserva um formato zerado no fim da lista. Ponteiros podem mudar até a
   extração terminar; depois são estáveis. */
cb_format *cb_media_add_format(cb_media *m);
void cb_media_free(cb_media *m);

#endif
