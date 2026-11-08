/*
 * Parser de playlists HLS (RFC 8216), no subconjunto usado na prática:
 *   master: EXT-X-STREAM-INF, EXT-X-MEDIA (TYPE=AUDIO)
 *   mídia : EXTINF, EXT-X-KEY (AES-128), EXT-X-MAP, EXT-X-MEDIA-SEQUENCE,
 *           EXT-X-ENDLIST
 * Todas as URIs saem resolvidas (absolutas) contra a URL da playlist.
 */
#ifndef CB_M3U8_H
#define CB_M3U8_H

#include <stddef.h>
#include <stdint.h>

#include "../core/arena.h"

typedef struct {
    char *uri;             /* absoluta */
    double duration;       /* segundos (EXTINF) */
    int64_t seq;           /* número de sequência (para IV implícito) */
    char *key_uri;         /* NULL = sem criptografia */
    int has_iv;
    uint8_t iv[16];
} cb_hls_segment;

typedef struct {
    char *uri;             /* absoluta */
    int64_t bandwidth;     /* bit/s */
    int width;
    int height;
    double fps;
    char *codecs;          /* NULL se ausente */
    char *audio_group;     /* NULL se ausente */
} cb_hls_variant;

typedef struct {
    char *group;
    char *name;
    char *uri;             /* absoluta, NULL se ausente */
    char *lang;
    int is_default;
} cb_hls_rendition;

typedef struct {
    int is_master;
    int ended;             /* tem #EXT-X-ENDLIST (VOD) */
    int64_t media_sequence;
    char *map_uri;         /* init segment (fMP4), NULL se ausente */

    cb_hls_segment *segments;
    size_t nseg;
    cb_hls_variant *variants;
    size_t nvariant;
    cb_hls_rendition *renditions;
    size_t nrendition;
} cb_m3u8;

/* Confere o cabeçalho "#EXTM3U" (ignora espaço inicial e BOM). */
int cb_m3u8_is_playlist(const char *data, size_t len);

/* Retorna 0 em sucesso. Em erro preenche err. */
int cb_m3u8_parse(cb_arena *a, const char *text, const char *base_url,
                  cb_m3u8 *out, char *err, size_t errlen);

#endif
