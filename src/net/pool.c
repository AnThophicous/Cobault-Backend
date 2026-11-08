#include "pool.h"

#include <pthread.h>
#include <stdlib.h>

typedef struct task {
    cb_task_fn fn;
    void *arg;
    struct task *next;
} task_t;

struct cb_pool {
    pthread_t *threads;
    int nthreads;
    cb_share *share;

    pthread_mutex_t mu;
    pthread_cond_t has_work;
    pthread_cond_t idle;

    task_t *head;
    task_t *tail;
    int active;
    int stop;
};

static void *worker(void *arg)
{
    cb_pool *p = arg;
    cb_http *h = cb_http_new(p->share);

    for (;;) {
        pthread_mutex_lock(&p->mu);
        while (!p->head && !p->stop)
            pthread_cond_wait(&p->has_work, &p->mu);
        if (!p->head && p->stop) {
            pthread_mutex_unlock(&p->mu);
            break;
        }

        task_t *t = p->head;
        p->head = t->next;
        if (!p->head)
            p->tail = NULL;
        p->active++;
        pthread_mutex_unlock(&p->mu);

        /* Tarefa roda sem o lock: só o worker dela toca no handle. */
        if (h)
            t->fn(t->arg, h);
        free(t);

        pthread_mutex_lock(&p->mu);
        p->active--;
        if (!p->head && p->active == 0)
            pthread_cond_broadcast(&p->idle);
        pthread_mutex_unlock(&p->mu);
    }

    cb_http_free(h);
    return NULL;
}

cb_pool *cb_pool_new(int nthreads, cb_share *share)
{
    if (nthreads < 1)
        nthreads = 1;

    cb_pool *p = calloc(1, sizeof *p);
    if (!p)
        return NULL;
    p->nthreads = nthreads;
    p->share = share;
    p->threads = calloc((size_t)nthreads, sizeof *p->threads);
    pthread_mutex_init(&p->mu, NULL);
    pthread_cond_init(&p->has_work, NULL);
    pthread_cond_init(&p->idle, NULL);
    if (!p->threads)
        goto fail;

    for (int i = 0; i < nthreads; i++) {
        if (pthread_create(&p->threads[i], NULL, worker, p) != 0) {
            p->nthreads = i;
            cb_pool_free(p);
            return NULL;
        }
    }
    return p;

fail:
    free(p);
    return NULL;
}

int cb_pool_submit(cb_pool *p, cb_task_fn fn, void *arg)
{
    task_t *t = malloc(sizeof *t);
    if (!t)
        return -1;
    t->fn = fn;
    t->arg = arg;
    t->next = NULL;

    pthread_mutex_lock(&p->mu);
    if (p->tail)
        p->tail->next = t;
    else
        p->head = t;
    p->tail = t;
    pthread_cond_signal(&p->has_work);
    pthread_mutex_unlock(&p->mu);
    return 0;
}

void cb_pool_wait(cb_pool *p)
{
    pthread_mutex_lock(&p->mu);
    while (p->head || p->active)
        pthread_cond_wait(&p->idle, &p->mu);
    pthread_mutex_unlock(&p->mu);
}

void cb_pool_free(cb_pool *p)
{
    if (!p)
        return;

    pthread_mutex_lock(&p->mu);
    p->stop = 1;
    pthread_cond_broadcast(&p->has_work);
    pthread_mutex_unlock(&p->mu);

    for (int i = 0; i < p->nthreads; i++)
        pthread_join(p->threads[i], NULL);

    pthread_mutex_destroy(&p->mu);
    pthread_cond_destroy(&p->has_work);
    pthread_cond_destroy(&p->idle);
    free(p->threads);
    free(p);
}
