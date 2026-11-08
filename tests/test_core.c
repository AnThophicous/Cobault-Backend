/*
 * Testes unitários do núcleo do Cobault (sem rede).
 *   make test
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/arena.h"
#include "core/json.h"
#include "core/util.h"
#include "extract/m3u8.h"
#include "media/format.h"
#include "media/media.h"

static int failures = 0;
static int checks = 0;

#define CHECK(cond)                                                    \
    do {                                                               \
        checks++;                                                      \
        if (!(cond)) {                                                 \
            failures++;                                                \
            fprintf(stderr, "FALHOU %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                              \
    } while (0)

#define CHECK_STR(got, want)                                           \
    do {                                                               \
        checks++;                                                      \
        const char *g_ = (got), *w_ = (want);                          \
        if (!g_ || strcmp(g_, w_) != 0) {                              \
            failures++;                                                \
            fprintf(stderr, "FALHOU %s:%d: esperado \"%s\", veio \"%s\"\n", \
                    __FILE__, __LINE__, w_, g_ ? g_ : "(null)");        \
        }                                                              \
    } while (0)

static void test_arena(void)
{
    cb_arena *a = cb_arena_new(64);
    CHECK(a != NULL);
    void *p1 = cb_arena_alloc(a, 3);
    void *p2 = cb_arena_alloc(a, 1000); /* maior que o bloco: novo bloco */
    CHECK(p1 && p2);
    CHECK(((uintptr_t)p1 % 16) == 0);
    CHECK(((uintptr_t)p2 % 16) == 0);
    char *s = cb_arena_strdup(a, "cobault");
    CHECK_STR(s, "cobault");
    char *t = cb_arena_strndup(a, "abcdef", 3);
    CHECK_STR(t, "abc");
    cb_arena_free(a);
}

static void test_json(void)
{
    cb_arena *a = cb_arena_new(0);
    char err[128];

    const char *src =
        "{\"a\": 1.5, \"b\": [true, false, null], \"c\": \"x\\ty\\u00e9\","
        " \"d\": {\"n\": \"123\"}, \"e\": \"\\ud83d\\ude00\"}";
    cb_json *root = cb_json_parse(a, src, strlen(src), err, sizeof err);
    CHECK(root != NULL);
    if (!root) {
        fprintf(stderr, "  erro: %s\n", err);
        cb_arena_free(a);
        return;
    }
    CHECK(cb_json_num(cb_json_get(root, "a"), 0) == 1.5);
    CHECK(cb_json_get(root, "b")->count == 3);
    CHECK_STR(cb_json_str(cb_json_get(root, "c")), "x\ty\xc3\xa9");
    CHECK(cb_json_i64(cb_json_get(cb_json_get(root, "d"), "n"), -1) == 123);
    CHECK(cb_json_i64(cb_json_get(root, "missing"), -7) == -7);
    /* Par substituto: U+1F600 em UTF-8 (4 bytes). */
    CHECK_STR(cb_json_str(cb_json_get(root, "e")), "\xf0\x9f\x98\x80");
    cb_arena_free(a);

    /* Erros devem ser detectados. */
    a = cb_arena_new(0);
    CHECK(cb_json_parse(a, "{\"a\": }", 7, err, sizeof err) == NULL);
    CHECK(cb_json_parse(a, "[1, 2", 5, err, sizeof err) == NULL);
    CHECK(cb_json_parse(a, "{} lixo", 7, err, sizeof err) == NULL);
    CHECK(cb_json_parse(a, "\"abc", 4, err, sizeof err) == NULL);
    cb_arena_free(a);
}

static void test_util(void)
{
    char *u;

    u = cb_url_resolve("https://h.com/a/b/index.m3u8", "seg1.ts");
    CHECK_STR(u, "https://h.com/a/b/seg1.ts");
    free(u);

    u = cb_url_resolve("https://h.com/a/b/index.m3u8", "/x/y.ts");
    CHECK_STR(u, "https://h.com/x/y.ts");
    free(u);

    u = cb_url_resolve("https://h.com/a/b.m3u8?token=1", "c.ts");
    CHECK_STR(u, "https://h.com/a/c.ts");
    free(u);

    u = cb_url_resolve("https://h.com/a/b.m3u8", "//cdn.com/z.ts");
    CHECK_STR(u, "https://cdn.com/z.ts");
    free(u);

    u = cb_url_resolve("https://h.com/a/b.m3u8", "https://other.net/q");
    CHECK_STR(u, "https://other.net/q");
    free(u);

    char ext[16];
    cb_url_ext("https://h.com/v/clip.MP4?x=1", ext, sizeof ext);
    CHECK_STR(ext, "mp4");

    int64_t n = 0;
    CHECK(cb_parse_size("512k", &n) == 0 && n == 512 * 1024);
    CHECK(cb_parse_size("4M", &n) == 0 && n == 4LL * 1024 * 1024);
    CHECK(cb_parse_size("2G", &n) == 0 && n == 2LL * 1024 * 1024 * 1024);
    CHECK(cb_parse_size("12", &n) == 0 && n == 12);
    CHECK(cb_parse_size("3X", &n) != 0);
    CHECK(cb_parse_size("", &n) != 0);

    cb_tpl_fields f = { .id = "abc", .title = "Aula: 1/2?", .ext = "mp4",
                        .extractor = "youtube", .format_id = "18", .height = 360 };
    char *out = cb_tpl_expand("%(title)s [%(id)s].%(ext)s", &f);
    CHECK_STR(out, "Aula_ 1_2_ [abc].mp4");
    free(out);

    out = cb_tpl_expand("Canais/%(extractor)s/%(height)s%%.%(ext)s", &f);
    CHECK_STR(out, "Canais/youtube/360%.mp4");
    free(out);

    char size[32];
    cb_fmt_size(1536, size, sizeof size);
    CHECK_STR(size, "1.5 KiB");
}

static void test_m3u8(void)
{
    cb_arena *a = cb_arena_new(0);
    char err[256];
    cb_m3u8 pl;

    const char *master =
        "#EXTM3U\n"
        "#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID=\"aud\",NAME=\"Português\",LANGUAGE=\"pt\","
        "DEFAULT=YES,URI=\"audio/pt.m3u8\"\n"
        "#EXT-X-STREAM-INF:BANDWIDTH=800000,RESOLUTION=640x360,FRAME-RATE=30.0,"
        "CODECS=\"avc1.4d401e,mp4a.40.2\"\n"
        "v360/index.m3u8\n"
        "#EXT-X-STREAM-INF:BANDWIDTH=2500000,RESOLUTION=1280x720,AUDIO=\"aud\","
        "CODECS=\"avc1.64001f\"\n"
        "v720/index.m3u8\n";
    CHECK(cb_m3u8_is_playlist(master, strlen(master)));
    CHECK(cb_m3u8_parse(a, master, "https://cdn.x/master.m3u8", &pl, err, sizeof err) == 0);
    CHECK(pl.is_master == 1);
    CHECK(pl.nvariant == 2);
    CHECK(pl.nrendition == 1);
    CHECK_STR(pl.variants[0].uri, "https://cdn.x/v360/index.m3u8");
    CHECK(pl.variants[0].height == 360 && pl.variants[0].width == 640);
    CHECK(pl.variants[0].bandwidth == 800000);
    CHECK_STR(pl.renditions[0].uri, "https://cdn.x/audio/pt.m3u8");
    CHECK_STR(pl.renditions[0].name, "Português");
    CHECK_STR(pl.variants[1].audio_group, "aud");
    cb_arena_free(a);

    const char *media =
        "#EXTM3U\n"
        "#EXT-X-VERSION:3\n"
        "#EXT-X-TARGETDURATION:6\n"
        "#EXT-X-MEDIA-SEQUENCE:10\n"
        "#EXT-X-MAP:URI=\"init.mp4\"\n"
        "#EXT-X-KEY:METHOD=AES-128,URI=\"keys/k1.bin\",IV=0x00000000000000000000000000000001\n"
        "#EXTINF:5.0,\n"
        "s10.ts\n"
        "#EXTINF:4.5,\n"
        "s11.ts\n"
        "#EXT-X-KEY:METHOD=NONE\n"
        "#EXTINF:3.0,\n"
        "https://other.cdn/s12.ts\n"
        "#EXT-X-ENDLIST\n";
    a = cb_arena_new(0);
    CHECK(cb_m3u8_parse(a, media, "https://cdn.x/p/index.m3u8?t=1", &pl, err, sizeof err) == 0);
    CHECK(pl.is_master == 0);
    CHECK(pl.ended == 1);
    CHECK(pl.nseg == 3);
    CHECK(pl.media_sequence == 10);
    CHECK_STR(pl.map_uri, "https://cdn.x/p/init.mp4");
    CHECK_STR(pl.segments[0].uri, "https://cdn.x/p/s10.ts");
    CHECK_STR(pl.segments[0].key_uri, "https://cdn.x/p/keys/k1.bin");
    CHECK(pl.segments[0].has_iv == 1 && pl.segments[0].iv[15] == 1 && pl.segments[0].iv[14] == 0);
    CHECK(pl.segments[0].seq == 10 && pl.segments[1].seq == 11);
    CHECK(pl.segments[1].duration > 4.4 && pl.segments[1].duration < 4.6);
    CHECK(pl.segments[2].key_uri == NULL);   /* METHOD=NONE limpa a chave */
    CHECK_STR(pl.segments[2].uri, "https://other.cdn/s12.ts");

    const char *bad = "#EXTM3U\n#EXT-X-KEY:METHOD=SAMPLE-AES,URI=\"k\"\n#EXTINF:1,\na.ts\n";
    CHECK(cb_m3u8_parse(a, bad, "https://x/y.m3u8", &pl, err, sizeof err) != 0);
    cb_arena_free(a);

    CHECK(!cb_m3u8_is_playlist("<html>", 6));
}

static cb_format *add(cb_media *m, const char *id, const char *ext, int has_v, int has_a,
                      int h, double tbr, int fps)
{
    cb_format *f = cb_media_add_format(m);
    f->format_id = cb_arena_strdup(m->arena, id);
    f->ext = cb_arena_strdup(m->arena, ext);
    f->has_video = has_v;
    f->has_audio = has_a;
    f->height = h;
    f->width = h ? h * 16 / 9 : 0;
    f->tbr = tbr;
    f->fps = fps;
    f->url = cb_arena_strdup(m->arena, "https://x/y");
    return f;
}

static void test_format(void)
{
    cb_media *m = cb_media_new("test");
    add(m, "18", "mp4", 1, 1, 360, 500, 30);
    add(m, "137", "mp4", 1, 0, 1080, 2500, 30);
    add(m, "247", "webm", 1, 0, 720, 1400, 30);
    add(m, "140", "m4a", 0, 1, 0, 128, 0);
    add(m, "251", "webm", 0, 1, 0, 160, 0);

    cb_selection sel;
    char err[256];

    CHECK(cb_format_select(m, "bv*+ba/b", &sel, err, sizeof err) == 0);
    CHECK(sel.video && sel.audio && sel.video->height == 1080);
    CHECK_STR(sel.audio->format_id, "251");

    CHECK(cb_format_select(m, "best", &sel, err, sizeof err) == 0);
    CHECK_STR(sel.single->format_id, "18");          /* muxado preferido */

    CHECK(cb_format_select(m, "bv[height<=720]", &sel, err, sizeof err) == 0);
    CHECK_STR(sel.single->format_id, "247");

    CHECK(cb_format_select(m, "bv+ba", &sel, err, sizeof err) == 0);
    CHECK(sel.video && sel.audio);

    CHECK(cb_format_select(m, "worst", &sel, err, sizeof err) == 0);
    CHECK_STR(sel.single->format_id, "18");

    CHECK(cb_format_select(m, "wv", &sel, err, sizeof err) == 0);
    CHECK_STR(sel.single->format_id, "247");       /* menor altura entre só-vídeo */

    CHECK(cb_format_select(m, "137", &sel, err, sizeof err) == 0);
    CHECK_STR(sel.single->format_id, "137");

    CHECK(cb_format_select(m, "ba[ext=m4a]", &sel, err, sizeof err) == 0);
    CHECK_STR(sel.single->format_id, "140");

    CHECK(cb_format_select(m, "b[format_id=247]", &sel, err, sizeof err) == 0);
    CHECK_STR(sel.single->format_id, "247");       /* o campo format_id tem '_' */
    CHECK(cb_format_select(m, "b[format_id=999]", &sel, err, sizeof err) != 0);
    CHECK(cb_format_check_syntax("b[format_id=18]", err, sizeof err) == 0);

    CHECK(cb_format_select(m, "nao-existe/ba[ext=flac]", &sel, err, sizeof err) != 0);
    CHECK(cb_format_select(m, "999/bv[height>=2000]/b", &sel, err, sizeof err) == 0);
    CHECK_STR(sel.single->format_id, "18");

    CHECK(cb_format_check_syntax("bv*+ba/b", err, sizeof err) == 0);
    CHECK(cb_format_check_syntax("best[height<<720]", err, sizeof err) != 0);
    CHECK(cb_format_check_syntax("best[altura<=720]", err, sizeof err) != 0);
    CHECK(cb_format_check_syntax("best+", err, sizeof err) != 0);

    cb_media_free(m);
}

int main(void)
{
    cb_log_level = CB_LOG_ERROR;
    test_arena();
    test_json();
    test_util();
    test_m3u8();
    test_format();

    printf("%d verificações, %d falhas\n", checks, failures);
    return failures ? 1 : 0;
}
