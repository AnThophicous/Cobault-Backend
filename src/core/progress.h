/*
 * Barra de progresso com velocidade suavizada e ETA.
 *
 * Lê o contador atômico cb_bytes_done (sem lock no caminho quente de
 * download) e redesenha a linha a cada 250 ms numa thread própria.
 */
#ifndef CB_PROGRESS_H
#define CB_PROGRESS_H

#include <stdint.h>

/* total < 0 significa tamanho desconhecido (mostra só bytes e velocidade). */
void cb_progress_start(const char *label, int64_t total);
void cb_progress_stop(void);

#endif
