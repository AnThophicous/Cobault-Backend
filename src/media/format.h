/*
 * Seletor de formatos.
 *
 * Gramática (subconjunto próprio, inspirado na sintaxe de -f):
 *
 *   spec    := alt ( '/' alt )*          primeira alternativa que casar vence
 *   alt     := part ( '+' part )?        '+' junta vídeo e áudio (merge)
 *   part    := name filter*
 *   name    := best|b|worst|w|bestvideo|bv|bv*|worstvideo|wv|wv*
 *            | bestaudio|ba|ba*|worstaudio|wa|wa*|<format_id>
 *   filter  := '[' key op value ']'
 *   key     := height|width|fps|tbr|filesize|ext|format_id
 *   op      := '<' | '<=' | '>' | '>=' | '=' | '!='
 *
 * "*" em bv/ba inclui formatos que também têm a outra trilha (muxados).
 */
#ifndef CB_FORMAT_H
#define CB_FORMAT_H

#include "../media/media.h"

typedef struct {
    const cb_format *video;   /* trilha de vídeo (ou formato único) */
    const cb_format *audio;   /* trilha de áudio quando é merge; senão NULL */
    const cb_format *single;  /* formato único; NULL quando é merge */
} cb_selection;

/* Retorna 0 se encontrou algo, -1 caso contrário (err preenchido com motivo). */
int cb_format_select(const cb_media *m, const char *spec, cb_selection *sel,
                     char *err, size_t errlen);

/* Imprime a tabela de formatos no stdout. */
void cb_format_print_table(const cb_media *m);

/* Valida a sintaxe sem precisar de formatos (usado para erro cedo). */
int cb_format_check_syntax(const char *spec, char *err, size_t errlen);

#endif
