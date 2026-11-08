#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../core/json.h"
#include "../core/util.h"
#include "extract.h"

/*
 * Clientes da API interna do player, em ordem de prioridade no modo auto.
 * Cada um tem contexto e User-Agent próprios.
 *
 *   VISIONOS  formatos adaptativos (vídeo e áudio) com URL direta e sem limite
 *             de faixa observado. Exige visitorData: player_fetch repete o
 *             pedido com o valor que a própria resposta recusada traz.
 *   IOS       os mesmos formatos, mas com faixa inicial limitada: cerca de 13 MiB
 *             em vídeo (o áudio corta antes, perto de 1 MiB). Offsets além do
 *             limite dão 403; a cauda de cada formato escolhido é testada antes.
 *   ANDROID   apenas o muxado 18.
 */
#define YT_IOS_RANGE_BUDGET (13LL * 1024 * 1024)

typedef struct {
    const char *name;
    const char *client_name;
    const char *client_version;
    const char *client_id;      /* X-YouTube-Client-Name */
    const char *user_agent;
    const char *device_ctx;     /* campos extras do contexto, sem chaves */
    int64_t range_budget;       /* bytes liberados em formatos adaptativos (0 = sem limite observado) */
} yt_client_t;

static const yt_client_t YT_CLIENTS[] = {
    {
        "visionos", "VISIONOS", "1.02", "101",
        "Mozilla/5.0 (Macintosh; Intel Mac OS X 15_7_3) AppleWebKit/605.1.15 "
        "(KHTML, like Gecko) Version/26.0 Safari/605.1.15",
        "\"deviceMake\":\"Apple\",\"deviceModel\":\"RealityDevice17,1\","
        "\"userAgent\":\"Mozilla/5.0 (Macintosh; Intel Mac OS X 15_7_3) AppleWebKit/605.1.15 "
        "(KHTML, like Gecko) Version/26.0 Safari/605.1.15\","
        "\"osName\":\"visionOS\",\"osVersion\":\"26.5.23O471\"",
        0,
    },
    {
        "ios", "IOS", "20.03.02", "5",
        "com.google.ios.youtube/20.03.02 (iPhone16,2; U; CPU iOS 18_2_1 like Mac OS X;)",
        "\"deviceMake\":\"Apple\",\"deviceModel\":\"iPhone16,2\","
        "\"osName\":\"iPhone\",\"osVersion\":\"18.2.1.22C161\"",
        YT_IOS_RANGE_BUDGET,
    },
    {
        "android", "ANDROID", "21.26.364", "3",
        "com.google.android.youtube/21.26.364 (Linux; U; Android 11) gzip",
        "\"androidSdkVersion\":30,\"osName\":\"Android\",\"osVersion\":\"11\"",
        0,
    },
};

#define YT_PLAYER_URL "https://www.youtube.com/youtubei/v1/player?prettyPrint=false"

int cb_is_youtube_url(const char *url)
{
    if (!cb_url_is_http(url))
        return 0;
    const char *host = strstr(url, "://") + 3;
    size_t hl = strcspn(host, "/?#:");
    char h[128];
    if (hl >= sizeof h)
        return 0;
    memcpy(h, host, hl);
    h[hl] = '\0';
    return strstr(h, "youtube.com") || strstr(h, "youtube-nocookie.com") ||
           strcmp(h, "youtu.be") == 0 || strcmp(h, "www.youtu.be") == 0;
}

static int is_id_char(char c)
{
    return isalnum((unsigned char)c) || c == '-' || c == '_';
}

/* Copia um id de 11 caracteres a partir de p. */
static int take_id(const char *p, char out[12])
{
    size_t n = 0;
    while (p[n] && is_id_char(p[n]))
        n++;
    if (n != 11)
        return 0;
    memcpy(out, p, 11);
    out[11] = '\0';
    return 1;
}

static int video_id_from_url(const char *url, char out[12])
{
    /* Parâmetro v= na query (watch?v=...). */
    const char *q = strchr(url, '?');
    if (q) {
        const char *p = q + 1;
        while (*p) {
            if (p[0] == 'v' && p[1] == '=' && (p == q + 1 || p[-1] == '&'))
                return take_id(p + 2, out);
            p = strchr(p, '&');
            if (!p)
                break;
            p++;
        }
    }
    static const char *const prefixes[] = { "youtu.be/", "/shorts/", "/embed/", "/live/", "/v/" };
    for (size_t i = 0; i < sizeof prefixes / sizeof prefixes[0]; i++) {
        const char *at = strstr(url, prefixes[i]);
        if (at)
            return take_id(at + strlen(prefixes[i]), out);
    }
    return 0;
}

/* "video/mp4; codecs=\"avc1.4D400C, mp4a.40.2\"" -> kind, ext, c1, c2. */
static void parse_mime(const char *mime, char *kind, size_t kn, char *ext, size_t en,
                       char *c1, size_t c1n, char *c2, size_t c2n)
{
    kind[0] = ext[0] = c1[0] = c2[0] = '\0';
    const char *slash = strchr(mime, '/');
    if (!slash)
        return;
    size_t kl = (size_t)(slash - mime);
    if (kl >= kn) kl = kn - 1;
    memcpy(kind, mime, kl);
    kind[kl] = '\0';

    const char *semi = strchr(slash, ';');
    size_t sl = (size_t)((semi ? semi : slash + strlen(slash)) - (slash + 1));
    char sub[16];
    if (sl >= sizeof sub) sl = sizeof sub - 1;
    memcpy(sub, slash + 1, sl);
    sub[sl] = '\0';

    /* Container: mp4/webm são as extensões reais usadas pelo YouTube. */
    if (strcmp(sub, "mp4") == 0)
        snprintf(ext, en, "%s", strcmp(kind, "audio") == 0 ? "m4a" : "mp4");
    else
        snprintf(ext, en, "%s", sub);

    const char *cq = strstr(mime, "codecs=\"");
    if (!cq)
        return;
    cq += 8;
    const char *end = strchr(cq, '"');
    if (!end)
        return;
    const char *comma = memchr(cq, ',', (size_t)(end - cq));
    size_t n1 = (size_t)((comma ? comma : end) - cq);
    if (n1 >= c1n) n1 = c1n - 1;
    memcpy(c1, cq, n1);
    c1[n1] = '\0';
    if (comma) {
        const char *c2s = comma + 1;
        while (*c2s == ' ')
            c2s++;
        size_t n2 = (size_t)(end - c2s);
        if (n2 >= c2n) n2 = c2n - 1;
        memcpy(c2, c2s, n2);
        c2[n2] = '\0';
    }
}

/* O IOS repete itags (variantes de idioma/DRC). Sufixo -2, -3... evita
   colisão de nomes de arquivo e permite selecionar cada um por id. */
static void unique_id(const cb_media *m, char *id, size_t n)
{
    size_t bl = strlen(id);
    int seen = 0;
    for (size_t k = 0; k < m->count; k++) {
        const char *e = m->formats[k].format_id;
        if (strncmp(e, id, bl) == 0 && (e[bl] == '\0' || e[bl] == '-'))
            seen++;
    }
    if (seen)
        snprintf(id + bl, n - bl, "-%d", seen + 1);
}

/* `client_start`: primeiro índice criado por este cliente. Itags já vindos de
   um cliente anterior são ignorados (o cliente anterior tem prioridade). */
static void add_format(cb_media *m, const yt_client_t *c, const cb_json *f, int muxed,
                       size_t client_start)
{
    cb_arena *a = m->arena;
    const cb_json *itag = cb_json_get(f, "itag");
    const char *url = cb_json_str(cb_json_get(f, "url"));
    const char *mime = cb_json_str(cb_json_get(f, "mimeType"));
    if (!url || !mime)
        return; /* sem URL direta: assinatura cifrada ou trilha SABR */

    char kind[16], ext[16], c1[64], c2[64];
    parse_mime(mime, kind, sizeof kind, ext, sizeof ext, c1, sizeof c1, c2, sizeof c2);
    if (kind[0] == '\0')
        return;

    char id[40];
    snprintf(id, sizeof id, "%lld", (long long)cb_json_i64(itag, 0));
    for (size_t k = 0; k < client_start; k++)
        if (strcmp(m->formats[k].format_id, id) == 0)
            return;
    unique_id(m, id, sizeof id);

    cb_format *fmt = cb_media_add_format(m);
    if (!fmt)
        return;
    fmt->format_id = cb_arena_strdup(a, id);
    fmt->url = cb_arena_strdup(a, url);
    fmt->ext = cb_arena_strdup(a, ext);
    fmt->user_agent = cb_arena_strdup(a, c->user_agent);
    fmt->width = (int)cb_json_i64(cb_json_get(f, "width"), 0);
    fmt->height = (int)cb_json_i64(cb_json_get(f, "height"), 0);
    fmt->fps = (int)cb_json_i64(cb_json_get(f, "fps"), 0);
    fmt->tbr = cb_json_num(cb_json_get(f, "bitrate"), 0.0) / 1000.0;
    fmt->filesize = cb_json_i64(cb_json_get(f, "contentLength"), 0);
    if (!muxed)
        fmt->max_bytes = c->range_budget;
    fmt->proto = CB_PROTO_HTTP;

    if (strcmp(kind, "video") == 0) {
        fmt->has_video = 1;
        fmt->vcodec = cb_arena_strdup(a, c1[0] ? c1 : "unknown");
        if (muxed) {
            fmt->has_audio = 1;
            fmt->acodec = cb_arena_strdup(a, c2[0] ? c2 : "unknown");
        } else {
            fmt->acodec = cb_arena_strdup(a, "none");
        }
    } else if (strcmp(kind, "audio") == 0) {
        fmt->has_audio = 1;
        fmt->acodec = cb_arena_strdup(a, c1[0] ? c1 : "unknown");
        fmt->vcodec = cb_arena_strdup(a, "none");
    } else {
        return;
    }
}

/* Copia um token (visitorData) só se tiver caracteres seguros para JSON e URL
   e couber em n. Valor fora disso falha, em vez de corromper o corpo. */
static int copy_token(const char *src, char *out, size_t n)
{
    if (!src || n < 2)
        return 0;
    size_t len = 0;
    for (; src[len]; len++) {
        char ch = src[len];
        if (len >= n - 1 ||
            !(isalnum((unsigned char)ch) || ch == '-' || ch == '_' || ch == '%' ||
              ch == '=' || ch == '+' || ch == '/'))
            return 0;
    }
    if (len == 0)
        return 0;
    memcpy(out, src, len + 1);
    return 1;
}

static int player_request(cb_http *h, const yt_client_t *c, const char *video_id,
                          const char *visitor, cb_arena *ja, cb_json **root,
                          char *err, size_t errlen)
{
    char vctx[1200] = "", vhdr[1100] = "";
    if (visitor && visitor[0]) {
        snprintf(vctx, sizeof vctx, "\"visitorData\":\"%s\",", visitor);
        snprintf(vhdr, sizeof vhdr, "X-Goog-Visitor-Id: %s", visitor);
    }
    char body[4096];
    snprintf(body, sizeof body,
             "{\"context\":{\"client\":{%s\"clientName\":\"%s\",\"clientVersion\":\"%s\","
             "%s,\"hl\":\"en\",\"gl\":\"US\"}},"
             "\"videoId\":\"%s\",\"contentCheckOk\":true,\"racyCheckOk\":true}",
             vctx, c->client_name, c->client_version, c->device_ctx, video_id);

    char hid[64], hver[80];
    snprintf(hid, sizeof hid, "X-YouTube-Client-Name: %s", c->client_id);
    snprintf(hver, sizeof hver, "X-YouTube-Client-Version: %s", c->client_version);
    const char *headers[4] = { hid, hver, NULL, NULL };
    if (vhdr[0])
        headers[2] = vhdr;

    cb_reqopts ro = {
        .user_agent = c->user_agent,
        .content_type = "application/json",
        .body = body,
        .body_len = strlen(body),
        .headers = headers,
        .text = 1,
    };
    cb_buf buf = { 0 };
    cb_resp resp;
    int rc = cb_http_get_mem_retry(h, YT_PLAYER_URL, &ro, &buf, &resp, 3);
    if (rc != CB_HTTP_OK) {
        char d[96];
        cb_http_describe(rc, resp.status, d, sizeof d);
        snprintf(err, errlen, "API do YouTube (%s): %s", c->name, d);
        cb_buf_free(&buf);
        return -1;
    }

    *root = cb_json_parse(ja, buf.data, buf.len, err, errlen);
    cb_buf_free(&buf);
    return *root ? 0 : -1;
}

/* Sem visitorData, o VISIONOS recusa o pedido (LOGIN_REQUIRED). A resposta
   recusada traz um visitorData: o pedido é repetido uma vez com ele. */
static int player_fetch(cb_http *h, const yt_client_t *c, const char *video_id, char *visitor,
                        size_t vn, cb_arena *ja, cb_json **root, char *err, size_t errlen)
{
    if (player_request(h, c, video_id, visitor, ja, root, err, errlen) != 0)
        return -1;
    if (strcmp(c->name, "visionos") != 0 || visitor[0])
        return 0;
    const char *status = cb_json_str(cb_json_get(cb_json_get(*root, "playabilityStatus"), "status"));
    if (status && strcmp(status, "OK") == 0)
        return 0;
    const char *rc_vd = cb_json_str(cb_json_get(cb_json_get(*root, "responseContext"), "visitorData"));
    if (!copy_token(rc_vd, visitor, vn))
        return 0;
    return player_request(h, c, video_id, visitor, ja, root, err, errlen);
}

cb_media *cb_youtube_extract(const cb_extract_opts *o, const char *url, char *err, size_t errlen)
{
    char vid[12];
    if (!video_id_from_url(url, vid)) {
        snprintf(err, errlen, "não foi possível achar o id do vídeo na URL");
        return NULL;
    }

    cb_media *m = cb_media_new("youtube");
    if (!m) {
        snprintf(err, errlen, "sem memória");
        return NULL;
    }
    cb_arena *a = m->arena;
    m->id = cb_arena_strdup(a, vid);
    char page[128];
    snprintf(page, sizeof page, "https://www.youtube.com/watch?v=%s", vid);
    m->webpage_url = cb_arena_strdup(a, page);

    char reasons[256] = "";
    int got_title = 0;

    for (size_t ci = 0; ci < sizeof YT_CLIENTS / sizeof YT_CLIENTS[0]; ci++) {
        const yt_client_t *c = &YT_CLIENTS[ci];
        if (strcmp(o->yt_client, "auto") != 0 && strcmp(o->yt_client, c->name) != 0)
            continue;

        cb_arena *ja = cb_arena_new(256 * 1024);
        cb_json *root = NULL;
        char perr[256] = "";
        char visitor[1024] = "";
        if (!ja || player_fetch(o->http, c, vid, visitor, sizeof visitor, ja, &root, perr, sizeof perr) != 0 || !root) {
            cb_debug("cliente %s: %s", c->name, perr[0] ? perr : "falha");
            snprintf(reasons + strlen(reasons), sizeof reasons - strlen(reasons),
                     "%s%s", reasons[0] ? "; " : "", perr[0] ? perr : c->name);
            cb_arena_free(ja);
            continue;
        }

        const cb_json *ps = cb_json_get(root, "playabilityStatus");
        const char *status = cb_json_str(cb_json_get(ps, "status"));
        if (!status || strcmp(status, "OK") != 0) {
            const char *reason = cb_json_str(cb_json_get(ps, "reason"));
            cb_debug("cliente %s recusou o vídeo: %s", c->name, status ? status : "?");
            snprintf(reasons + strlen(reasons), sizeof reasons - strlen(reasons),
                     "%s%s", reasons[0] ? "; " : "",
                     reason ? reason : (status ? status : "indisponível"));
            cb_arena_free(ja);
            continue;
        }

        if (!got_title) {
            const cb_json *vd = cb_json_get(root, "videoDetails");
            const char *title = cb_json_str(cb_json_get(vd, "title"));
            m->title = cb_arena_strdup(a, title ? title : "");
            got_title = 1;
        }

        const cb_json *sd = cb_json_get(root, "streamingData");
        size_t client_start = m->count;
        int skipped = 0;

        const cb_json *list[] = { cb_json_get(sd, "formats"), cb_json_get(sd, "adaptiveFormats") };
        for (int li = 0; li < 2; li++) {
            if (!list[li] || list[li]->type != CB_JSON_ARRAY)
                continue;
            for (const cb_json *f = list[li]->first; f; f = f->next) {
                if (cb_json_get(f, "signatureCipher") || cb_json_get(f, "cipher")) {
                    skipped++;
                    continue;
                }
                add_format(m, c, f, li == 0, client_start);
            }
        }

        const char *hls = cb_json_str(cb_json_get(sd, "hlsManifestUrl"));
        if (hls) {
            char herr[256] = "";
            if (cb_extract_hls_manifest(m, o->http, hls, NULL, herr, sizeof herr) != 0)
                cb_debug("manifesto HLS do YouTube ignorado: %s", herr);
        }

        if (skipped)
            cb_debug("%d formato(s) com assinatura cifrada ignorados (cliente %s)", skipped, c->name);

        cb_arena_free(ja);
        /* Sem break no modo auto: cada cliente pode acrescentar formatos. */
    }

    if (m->count == 0) {
        snprintf(err, errlen, "nenhum formato baixável: %s",
                 reasons[0] ? reasons : "o vídeo pode exigir login ou PO token");
        cb_media_free(m);
        return NULL;
    }
    return m;
}
