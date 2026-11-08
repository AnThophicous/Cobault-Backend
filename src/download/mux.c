/*
 * Mescla vídeo e áudio com ffmpeg -c copy (sem recodificar). Usa fork e
 * execvp sem shell: nomes de arquivo com espaços, aspas ou ';' são seguros.
 */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "download.h"

int cb_mux_streams(const char *ffmpeg, const char *video, const char *audio,
                   const char *out, char *err, size_t errlen)
{
    if (!ffmpeg) {
        snprintf(err, errlen, "ffmpeg não configurado");
        return -1;
    }

    char *argv[] = {
        (char *)ffmpeg,
        "-hide_banner", "-loglevel", "error", "-nostdin", "-y",
        "-i", (char *)video,
        "-i", (char *)audio,
        "-map", "0:v:0", "-map", "1:a:0",
        "-c", "copy",
        (char *)out,
        NULL,
    };

    pid_t pid = fork();
    if (pid < 0) {
        snprintf(err, errlen, "fork falhou: %s", strerror(errno));
        return -1;
    }
    if (pid == 0) {
        execvp(ffmpeg, argv);
        _exit(127);
    }

    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) {
            snprintf(err, errlen, "waitpid falhou: %s", strerror(errno));
            return -1;
        }
    }
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
        return 0;
    if (WIFEXITED(status) && WEXITSTATUS(status) == 127)
        snprintf(err, errlen, "não encontrei o ffmpeg (%s)", ffmpeg);
    else
        snprintf(err, errlen, "ffmpeg terminou com erro (código %d)",
                 WIFEXITED(status) ? WEXITSTATUS(status) : -1);
    return -1;
}
