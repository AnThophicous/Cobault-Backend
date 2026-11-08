#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../core/util.h"
#include "extract.h"
#include "m3u8.h"

/* Baixa um texto pequeno (playlist) com retentativas. */
static int fetch_text(cb_http *h, const char *url, cb_buf *buf, char *err, size_t errlen)
{
    cb_reqopts ro = { .text = 1 };
    cb_resp resp;
    int rc = cb_http_get_mem_retry(h, url, &ro, buf, &resp, 3);
    if (rc != CB_HTTP_OK) {
        char d[96];
        cb_http_describe(rc, resp.status, d, sizeof d);
        snprintf(err, errlen, "falha ao baixar %s: %s", url, d);
        return -1;
    }
    return 0;
}

static int codecs_has(const char *codecs, const char *const *needles)
{
    if (!codecs)
        return 0;
    for (; *needles; needles++)
        if (strstr(codecs, *needles))
            return 1;
    return 0;
}

/* Copia o primeiro item de "codecs" (lista separada por vírgulas) cujo
   prefixo está em needles. Deixa out vazio se nenhum casar. */
static void pick_codec(const char *codecs, const char *const *needles, char *out, size_t n)
{
    out[0] = '\0';
    if (!codecs)
        return;
    const char *p = codecs;
    while (*p) {
        const char *e = strchr(p, ',');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        while (len && *p == ' ') {
            p++;
            len--;
        }
        for (const char *const *nd = needles; *nd; nd++) {
            size_t nl = strlen(*nd);
            if (len >= nl && memcmp(p, *nd, nl) == 0) {
                snprintf(out, n, "%.*s", (int)len, p);
                return;
            }
        }
        if (!e)
            break;
        p = e + 1;
    }
}

int cb_extract_hls_manifest(cb_media *m, cb_http *h, const char *manifest_url,
                            const char *text, char *err, size_t errlen)
{
    cb_buf buf = { 0 };
    const char *body = text;
    if (!body) {
        if (fetch_text(h, manifest_url, &buf, err, errlen) != 0)
            return -1;
        body = buf.data;
    }

    cb_m3u8 pl;
    if (cb_m3u8_parse(m->arena, body, manifest_url, &pl, err, errlen) != 0) {
        cb_buf_free(&buf);
        return -1;
    }
    cb_arena *a = m->arena;

    if (!pl.is_master) {
        /* Playlist de mídia direta: um único formato com todos os segmentos. */
        cb_format *f = cb_media_add_format(m);
        if (f) {
            f->format_id = cb_arena_strdup(a, "hls");
            f->url = cb_arena_strdup(a, manifest_url);
            f->ext = cb_arena_strdup(a, "ts");
            f->proto = CB_PROTO_HLS;
            f->has_video = 1;
            f->has_audio = 1;
            f->vcodec = cb_arena_strdup(a, "unknown");
            f->acodec = cb_arena_strdup(a, "unknown");
        }
        cb_buf_free(&buf);
        return 0;
    }

    /* Renditions de áudio alternativo (EXT-X-MEDIA TYPE=AUDIO). */
    for (size_t i = 0; i < pl.nrendition; i++) {
        cb_hls_rendition *r = &pl.renditions[i];
        if (!r->uri)
            continue;
        cb_format *f = cb_media_add_format(m);
        if (!f)
            break;
        char id[48];
        snprintf(id, sizeof id, "hls-audio-%zu", i);
        f->format_id = cb_arena_strdup(a, id);
        f->url = cb_arena_strdup(a, r->uri);
        f->ext = cb_arena_strdup(a, "m4a");
        f->proto = CB_PROTO_HLS;
        f->has_audio = 1;
        f->acodec = cb_arena_strdup(a, "unknown");
        f->vcodec = cb_arena_strdup(a, "none");
    }

    static const char *const video_codecs[] = { "avc1", "hvc1", "hev1", "vp09", "av01", NULL };
    static const char *const audio_codecs[] = { "mp4a", "opus", "ac-3", "ec-3", "flac", NULL };

    for (size_t i = 0; i < pl.nvariant; i++) {
        cb_hls_variant *v = &pl.variants[i];
        cb_format *f = cb_media_add_format(m);
        if (!f)
            break;
        char id[48];
        snprintf(id, sizeof id, "hls-%zu", i);
        f->format_id = cb_arena_strdup(a, id);
        f->url = cb_arena_strdup(a, v->uri);
        f->ext = cb_arena_strdup(a, "ts");
        f->proto = CB_PROTO_HLS;
        f->width = v->width;
        f->height = v->height;
        f->fps = (int)(v->fps + 0.5);
        f->tbr = (double)v->bandwidth / 1000.0;

        int has_v = v->width > 0 || codecs_has(v->codecs, video_codecs);
        /* Áudio em rendition separada: a variante é só vídeo. */
        int has_a = !v->audio_group && codecs_has(v->codecs, audio_codecs);
        f->has_video = has_v;
        f->has_audio = has_a || (!v->codecs && !has_v);
        char vc[64] = "", ac[64] = "";
        pick_codec(v->codecs, video_codecs, vc, sizeof vc);
        pick_codec(v->codecs, audio_codecs, ac, sizeof ac);
        f->vcodec = cb_arena_strdup(a, has_v ? (vc[0] ? vc : "unknown") : "none");
        if (f->has_audio)
            f->acodec = cb_arena_strdup(a, ac[0] ? ac : "unknown");
        else
            f->acodec = cb_arena_strdup(a, v->audio_group ? "externo" : "none");
    }

    cb_buf_free(&buf);
    return 0;
}

/* Arquivo HTTP direto: sondagem com Range para detectar HLS e tamanho. */
static cb_media *extract_generic(const cb_extract_opts *o, const char *url, char *err, size_t errlen)
{
    cb_http *h = o->http;
    cb_buf probe = { 0 };
    cb_reqopts ro = { .range = "0-511", .max_bytes = 512 };
    cb_resp resp;
    int rc = cb_http_get_mem_retry(h, url, &ro, &probe, &resp, 3);
    if (rc != CB_HTTP_OK) {
        char d[96];
        cb_http_describe(rc, resp.status, d, sizeof d);
        snprintf(err, errlen, "não consegui acessar a URL: %s", d);
        cb_buf_free(&probe);
        return NULL;
    }

    cb_media *m = cb_media_new("generic");
    if (!m) {
        snprintf(err, errlen, "sem memória");
        cb_buf_free(&probe);
        return NULL;
    }
    char name[256], ext[16];
    cb_url_basename(url, name, sizeof name);
    cb_url_ext(url, ext, sizeof ext);
    char *dot = strrchr(name, '.');
    if (dot && dot != name)
        *dot = '\0';
    m->id = cb_arena_strdup(m->arena, name);
    m->title = cb_arena_strdup(m->arena, name);
    m->webpage_url = cb_arena_strdup(m->arena, url);

    if (cb_m3u8_is_playlist(probe.data ? probe.data : "", probe.len)) {
        cb_buf_free(&probe);
        if (cb_extract_hls_manifest(m, h, url, NULL, err, errlen) != 0 || m->count == 0) {
            if (m->count == 0 && !err[0])
                snprintf(err, errlen, "playlist sem formatos");
            cb_media_free(m);
            return NULL;
        }
        return m;
    }

    cb_format *f = cb_media_add_format(m);
    if (!f) {
        snprintf(err, errlen, "sem memória");
        cb_media_free(m);
        cb_buf_free(&probe);
        return NULL;
    }
    f->format_id = cb_arena_strdup(m->arena, "direct");
    f->url = cb_arena_strdup(m->arena, url);
    f->ext = cb_arena_strdup(m->arena, ext[0] ? ext : "bin");
    f->proto = CB_PROTO_HTTP;
    f->has_video = 1;
    f->has_audio = 1;
    if (resp.range_total > 0)
        f->filesize = resp.range_total;
    else if (resp.status == 200 && resp.content_length > 0)
        f->filesize = resp.content_length;

    cb_buf_free(&probe);
    return m;
}

cb_media *cb_extract(const cb_extract_opts *o, const char *url, char *err, size_t errlen)
{
    if (!cb_url_is_http(url)) {
        snprintf(err, errlen, "a URL precisa começar com http:// ou https://");
        return NULL;
    }
    if (cb_is_youtube_url(url))
        return cb_youtube_extract(o, url, err, errlen);
    return extract_generic(o, url, err, errlen);
}
