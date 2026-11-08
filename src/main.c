/*
 * Ponto de entrada da linha de comando.
 */
#include <getopt.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "core/progress.h"
#include "core/util.h"
#include "download/download.h"
#include "extract/extract.h"
#include "media/format.h"
#include "net/http.h"

#define COBAULT_VERSION "1.0.0"
#define DEFAULT_SPEC "bv+ba/b"
#define DEFAULT_TEMPLATE "%(title)s [%(id)s].%(ext)s"

typedef struct {
    const char *spec;
    const char *tpl;
    const char *yt_client;
    const char *ffmpeg;
    int list_only;
    int no_merge;
    int jobs;
    int retries;
    int64_t chunk_size;
} cli_t;

static void on_sigint(int sig)
{
    (void)sig;
    cb_interrupted = 1;
}

static void usage(FILE *out)
{
    fprintf(out,
        "Cobault %s: baixador de mídia nativo em C\n\n"
        "uso: cobault [opções] URL [URL ...]\n\n"
        "  -F, --list-formats        lista os formatos disponíveis e sai\n"
        "  -f, --format SEL          seletor de formato (padrão: %s)\n"
        "  -o, --output MODELO       nome de saída (padrão: \"%s\")\n"
        "                            campos: %%(id)s %%(title)s %%(ext)s %%(extractor)s\n"
        "                                    %%(format_id)s %%(height)s\n"
        "  -N, --jobs N              conexões paralelas por download (padrão: 8)\n"
        "  -s, --chunk-size TAM      bloco de Range, ex. 512k, 1M (padrão: 1M)\n"
        "  -R, --retries N           tentativas extras por requisição (padrão: 5)\n"
        "      --client NOME         cliente do YouTube: auto, visionos, ios, android\n"
        "      --ffmpeg CAMINHO      ffmpeg usado para juntar vídeo e áudio\n"
        "      --no-merge            mantém vídeo e áudio em arquivos separados\n"
        "  -q, --quiet               só erros\n"
        "  -v, --verbose             mensagens de depuração\n"
        "  -V, --version             mostra a versão\n"
        "  -h, --help                esta ajuda\n\n"
        "Seletor: best, worst, bv, ba (use * para incluir muxados), format_id,\n"
        "         filtros [height<=720] [ext=mp4]; '+' junta vídeo e áudio; '/' é fallback.\n",
        COBAULT_VERSION, DEFAULT_SPEC, DEFAULT_TEMPLATE);
}

static const struct option LONG_OPTS[] = {
    { "list-formats", no_argument, NULL, 'F' },
    { "format", required_argument, NULL, 'f' },
    { "output", required_argument, NULL, 'o' },
    { "jobs", required_argument, NULL, 'N' },
    { "chunk-size", required_argument, NULL, 's' },
    { "retries", required_argument, NULL, 'R' },
    { "client", required_argument, NULL, 1001 },
    { "ffmpeg", required_argument, NULL, 1002 },
    { "no-merge", no_argument, NULL, 1003 },
    { "quiet", no_argument, NULL, 'q' },
    { "verbose", no_argument, NULL, 'v' },
    { "version", no_argument, NULL, 'V' },
    { "help", no_argument, NULL, 'h' },
    { NULL, 0, NULL, 0 },
};

/* Verdadeiro se o ffmpeg existe e é executável, procurando no PATH. */
static int ffmpeg_available(const char *name)
{
    if (strchr(name, '/'))
        return access(name, X_OK) == 0;
    const char *path = getenv("PATH");
    if (!path)
        return 0;
    char *copy = strdup(path);
    if (!copy)
        return 0;
    int found = 0;
    for (char *tok = strtok(copy, ":"); tok && !found; tok = strtok(NULL, ":")) {
        char full[1024];
        snprintf(full, sizeof full, "%s/%s", tok, name);
        found = access(full, X_OK) == 0;
    }
    free(copy);
    return found;
}

static int parse_args(int argc, char **argv, cli_t *c)
{
    int opt;
    while ((opt = getopt_long(argc, argv, "Ff:o:N:s:R:qvVh", LONG_OPTS, NULL)) != -1) {
        switch (opt) {
        case 'F': c->list_only = 1; break;
        case 'f': c->spec = optarg; break;
        case 'o': c->tpl = optarg; break;
        case 'N':
            c->jobs = atoi(optarg);
            if (c->jobs < 1 || c->jobs > 64) {
                cb_error("-N precisa estar entre 1 e 64");
                return -1;
            }
            break;
        case 's':
            if (cb_parse_size(optarg, &c->chunk_size) != 0 ||
                c->chunk_size < 64 * 1024 || c->chunk_size > 64LL * 1024 * 1024) {
                cb_error("-s aceita entre 64k e 64M (ex.: 1M)");
                return -1;
            }
            break;
        case 'R':
            c->retries = atoi(optarg);
            if (c->retries < 0 || c->retries > 20) {
                cb_error("-R precisa estar entre 0 e 20");
                return -1;
            }
            break;
        case 1001:
            if (strcmp(optarg, "auto") != 0 && strcmp(optarg, "visionos") != 0 &&
                strcmp(optarg, "ios") != 0 && strcmp(optarg, "android") != 0) {
                cb_error("--client aceita: auto, visionos, ios, android");
                return -1;
            }
            c->yt_client = optarg;
            break;
        case 1002: c->ffmpeg = optarg; break;
        case 1003: c->no_merge = 1; break;
        case 'q': cb_log_level = CB_LOG_ERROR; break;
        case 'v': cb_log_level = CB_LOG_DEBUG; break;
        case 'V': printf("Cobault %s\n", COBAULT_VERSION); exit(0);
        case 'h': usage(stdout); exit(0);
        default:
            usage(stderr);
            return -1;
        }
    }
    return optind;
}

static const char *proto_label(const cb_format *f)
{
    return f->proto == CB_PROTO_HLS ? "HLS" : "HTTP";
}

/* Baixa um formato, pulando se o destino final já existir. */
static int download_one(const cb_dl_opts *dl, cb_share *share, const cb_format *f, const char *path)
{
    struct stat st;
    if (stat(path, &st) == 0) {
        cb_info("já existe, pulando: %s", path);
        return 0;
    }
    cb_info("formato %s (%s, %s) → %s", f->format_id, f->ext, proto_label(f), path);

    char err[512] = "";
    if (cb_download_format(dl, share, f, path, err, sizeof err) != 0) {
        cb_error("%s", err);
        return -1;
    }
    return 0;
}

/* Caminho sem a extensão final: "pasta/a.b.mkv" -> "pasta/a.b". */
static void strip_ext(const char *path, char *out, size_t n)
{
    snprintf(out, n, "%s", path);
    char *slash = strrchr(out, '/');
    char *dot = strrchr(out, '.');
    if (dot && (!slash || dot > slash))
        *dot = '\0';
}

static const char *mux_ext(const cb_format *v, const cb_format *a)
{
    int mp4_v = strcmp(v->ext, "mp4") == 0;
    int mp4_a = strcmp(a->ext, "m4a") == 0 || strcmp(a->ext, "mp4") == 0;
    return (mp4_v && mp4_a) ? "mp4" : "mkv";
}

static int process_url(const cli_t *c, cb_share *share, cb_http *eh, const char *url)
{
    char err[512] = "";
    cb_extract_opts eo = { .http = eh, .yt_client = c->yt_client };

    cb_media *m = cb_extract(&eo, url, err, sizeof err);
    if (!m) {
        cb_error("%s", err);
        return 1;
    }
    if (c->list_only) {
        cb_format_print_table(m);
        cb_media_free(m);
        return 0;
    }

    /* Seleciona, testa a cauda dos formatos escolhidos e, se algum falhar,
       marca como indisponível e refaz a seleção (cada rodada exclui pelo menos
       um formato, então o laço termina). */
    cb_selection sel;
    for (;;) {
        if (cb_format_select(m, c->spec, &sel, err, sizeof err) != 0) {
            cb_error("%s", err);
            cb_media_free(m);
            return 1;
        }
        const cb_format *chosen[3] = { sel.single, sel.video, sel.audio };
        cb_format *bad = NULL;
        for (int k = 0; k < 3 && !bad; k++) {
            if (!chosen[k])
                continue;
            cb_format *cand = &m->formats[chosen[k] - m->formats];
            if (!cb_probe_tail(eh, cand))
                bad = cand;
        }
        if (!bad)
            break;
        cb_info("formato %s não entrega o arquivo inteiro; procurando outra opção",
                bad->format_id);
    }

    int can_mux = !c->no_merge && sel.video && sel.audio && ffmpeg_available(c->ffmpeg);
    if (sel.video && sel.audio && !can_mux && !c->no_merge)
        cb_warn("ffmpeg não encontrado: mantendo vídeo e áudio em arquivos separados");

    cb_dl_opts dl = {
        .jobs = c->jobs,
        .chunk_size = (size_t)c->chunk_size,
        .retries = c->retries,
        .ffmpeg = can_mux ? c->ffmpeg : NULL,
    };

    int rc = 0;
    if (sel.single) {
        cb_tpl_fields tf = {
            .id = m->id, .title = m->title, .ext = sel.single->ext,
            .extractor = m->extractor, .format_id = sel.single->format_id,
            .height = sel.single->height,
        };
        char *path = cb_tpl_expand(c->tpl, &tf);
        if (!path) {
            cb_error("sem memória");
            cb_media_free(m);
            return 1;
        }
        rc = download_one(&dl, share, sel.single, path);
        if (rc == 0)
            printf("%s\n", path);
        free(path);
    } else if (can_mux) {
        char merged_id[64];
        snprintf(merged_id, sizeof merged_id, "%s+%s", sel.video->format_id, sel.audio->format_id);
        cb_tpl_fields tf = {
            .id = m->id, .title = m->title, .ext = mux_ext(sel.video, sel.audio),
            .extractor = m->extractor, .format_id = merged_id,
            .height = sel.video->height,
        };
        char *final = cb_tpl_expand(c->tpl, &tf);
        if (!final) {
            cb_error("sem memória");
            cb_media_free(m);
            return 1;
        }

        /* Trilhas temporárias ao lado do destino, com o id do formato. */
        char stem[1100], tv[1200], ta[1200];
        strip_ext(final, stem, sizeof stem);
        snprintf(tv, sizeof tv, "%s.f%s.%s", stem, sel.video->format_id, sel.video->ext);
        snprintf(ta, sizeof ta, "%s.f%s.%s", stem, sel.audio->format_id, sel.audio->ext);

        struct stat st;
        if (stat(final, &st) == 0) {
            cb_info("já existe, pulando: %s", final);
            printf("%s\n", final);
        } else if (download_one(&dl, share, sel.video, tv) == 0 &&
                   download_one(&dl, share, sel.audio, ta) == 0) {
            char err2[512] = "";
            cb_info("juntando vídeo e áudio com ffmpeg → %s", final);
            if (cb_mux_streams(c->ffmpeg, tv, ta, final, err2, sizeof err2) == 0) {
                unlink(tv);
                unlink(ta);
                printf("%s\n", final);
            } else {
                cb_error("%s (trilhas mantidas: %s e %s)", err2, tv, ta);
                rc = -1;
            }
        } else {
            rc = -1;
        }
        free(final);
    } else {
        /* Sem merge: uma trilha por arquivo. Se o modelo dá o mesmo nome às duas
           (mesma extensão e sem %(format_id)s), a segunda recebe o id do formato
           antes da extensão, para não ser pulada como "já existe". */
        cb_tpl_fields tf = {
            .id = m->id, .title = m->title, .ext = NULL,
            .extractor = m->extractor, .format_id = NULL, .height = 0,
        };
        const cb_format *parts[2] = { sel.video, sel.audio };
        char *first = NULL;
        for (int k = 0; k < 2 && rc == 0; k++) {
            const cb_format *f = parts[k];
            if (!f)
                continue;
            tf.ext = f->ext;
            tf.format_id = f->format_id;
            tf.height = f->height;
            char *path = cb_tpl_expand(c->tpl, &tf);
            if (!path) {
                rc = -1;
                break;
            }
            if (first && strcmp(path, first) == 0) {
                char stem[1100], unique[1200];
                strip_ext(path, stem, sizeof stem);
                snprintf(unique, sizeof unique, "%s.f%s.%s", stem, f->format_id, f->ext);
                free(path);
                path = strdup(unique);
                if (!path) {
                    rc = -1;
                    break;
                }
            }
            rc = download_one(&dl, share, f, path) == 0 ? 0 : -1;
            if (rc == 0)
                printf("%s\n", path);
            if (first)
                free(path);
            else
                first = path;
        }
        free(first);
    }

    cb_media_free(m);
    return rc == 0 ? 0 : 1;
}

int main(int argc, char **argv)
{
    cli_t c = {
        .spec = DEFAULT_SPEC,
        .tpl = DEFAULT_TEMPLATE,
        .yt_client = "auto",
        .ffmpeg = "ffmpeg",
        .jobs = 8,
        .retries = 5,
        .chunk_size = 1024 * 1024,
    };

    int first = parse_args(argc, argv, &c);
    if (first < 0)
        return 2;
    if (first >= argc) {
        usage(stderr);
        return 2;
    }

    char err[512] = "";
    if (cb_format_check_syntax(c.spec, err, sizeof err) != 0) {
        cb_error("seletor de formato inválido: %s", err);
        return 2;
    }

    struct sigaction sa = { 0 };
    sa.sa_handler = on_sigint;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    if (cb_http_global_init() != 0) {
        cb_error("falha ao iniciar o libcurl");
        return 1;
    }

    cb_share *share = cb_share_new();
    cb_http *eh = share ? cb_http_new(share) : NULL;
    if (!eh) {
        cb_error("sem memória");
        cb_http_global_cleanup();
        return 1;
    }

    int failures = 0;
    for (int i = first; i < argc && !cb_interrupted; i++) {
        if (process_url(&c, share, eh, argv[i]) != 0)
            failures++;
    }

    cb_progress_stop();
    cb_http_free(eh);
    cb_share_free(share);
    cb_http_global_cleanup();

    if (cb_interrupted) {
        cb_error("interrompido pelo usuário");
        return 130;
    }
    return failures ? 1 : 0;
}
