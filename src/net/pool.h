/*
 * Pool de threads com fila FIFO. Cada worker cria um cb_http no início e o
 * usa em todas as tarefas (keep-alive por thread). A ordem FIFO importa para
 * o escritor de HLS: tarefas de índice menor começam antes das maiores.
 */
#ifndef CB_POOL_H
#define CB_POOL_H

#include "http.h"

typedef struct cb_pool cb_pool;
typedef void (*cb_task_fn)(void *arg, cb_http *h);

cb_pool *cb_pool_new(int nthreads, cb_share *share);
/* Retorna 0 em sucesso. */
int cb_pool_submit(cb_pool *p, cb_task_fn fn, void *arg);
/* Bloqueia até a fila esvaziar e todos os workers ficarem ociosos. */
void cb_pool_wait(cb_pool *p);
/* Encerra: espera as tarefas pendentes e junta as threads. */
void cb_pool_free(cb_pool *p);

#endif
