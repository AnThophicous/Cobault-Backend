#include "progress.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "util.h"
#include "../net/http.h"

#define BAR_WIDTH 28

static pthread_t ticker;
static pthread_mutex_t state_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t state_cv = PTHREAD_COND_INITIALIZER;
static int running = 0;
static int64_t total_bytes = -1;
static char label[160];

static void format_eta(double secs, char *buf, size_t n)
{
    if (secs < 0 || secs > 359999) {
        snprintf(buf, n, "--:--");
        return;
    }
    int s = (int)secs;
    snprintf(buf, n, "%02d:%02d", s / 60, s % 60);
}

static void draw(double rate, uint64_t done)
{
    char line[512];
    char dsize[32], drate[32], eta[16];
    cb_fmt_size((double)done, dsize, sizeof dsize);
    cb_fmt_size(rate, drate, sizeof drate);

    if (total_bytes > 0) {
        double frac = (double)done / (double)total_bytes;
        if (frac > 1.0) frac = 1.0;
        int filled = (int)(frac * BAR_WIDTH);
        char bar[BAR_WIDTH + 1];
        for (int i = 0; i < BAR_WIDTH; i++)
            bar[i] = i < filled ? '#' : '-';
        bar[BAR_WIDTH] = '\0';

        double remaining = rate > 0 ? (double)(total_bytes - (int64_t)done) / rate : -1;
        format_eta(remaining, eta, sizeof eta);
        char dtotal[32];
        cb_fmt_size((double)total_bytes, dtotal, sizeof dtotal);
        snprintf(line, sizeof line, "%s [%s] %5.1f%%  %s/%s  %s/s  ETA %s",
                 label, bar, frac * 100.0, dsize, dtotal, drate, eta);
    } else {
        snprintf(line, sizeof line, "%s  %s  %s/s", label, dsize, drate);
    }

    pthread_mutex_lock(&cb_console_lock);
    fprintf(stderr, "\r\033[K%s", line);
    fflush(stderr);
    cb_console_progress_active = 1;
    pthread_mutex_unlock(&cb_console_lock);
}

static void *tick(void *arg)
{
    (void)arg;
    double t_prev = cb_now();
    uint64_t b_prev = 0;
    double rate = 0.0;

    pthread_mutex_lock(&state_mu);
    while (running) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 250 * 1000000L;
        if (ts.tv_nsec >= 1000000000L) {
            ts.tv_sec++;
            ts.tv_nsec -= 1000000000L;
        }
        pthread_cond_timedwait(&state_cv, &state_mu, &ts);
        if (!running)
            break;
        pthread_mutex_unlock(&state_mu);

        double now = cb_now();
        uint64_t done = atomic_load(&cb_bytes_done);
        double dt = now - t_prev;
        if (dt > 0) {
            double inst = (double)(done - b_prev) / dt;
            /* Média móvel exponencial: estável sem perder reação. */
            rate = rate == 0.0 ? inst : rate * 0.7 + inst * 0.3;
        }
        t_prev = now;
        b_prev = done;
        draw(rate, done);

        pthread_mutex_lock(&state_mu);
    }
    pthread_mutex_unlock(&state_mu);
    return NULL;
}

void cb_progress_start(const char *lbl, int64_t total)
{
    if (cb_log_level < CB_LOG_INFO || !isatty(2))
        return;

    total_bytes = total;
    snprintf(label, sizeof label, "%s", lbl);
    atomic_store(&cb_bytes_done, 0);

    pthread_mutex_lock(&state_mu);
    running = 1;
    pthread_mutex_unlock(&state_mu);
    pthread_create(&ticker, NULL, tick, NULL);
}

void cb_progress_stop(void)
{
    if (!cb_console_progress_active && !running)
        return;

    pthread_mutex_lock(&state_mu);
    int was_running = running;
    running = 0;
    pthread_cond_signal(&state_cv);
    pthread_mutex_unlock(&state_mu);

    if (was_running)
        pthread_join(ticker, NULL);

    pthread_mutex_lock(&cb_console_lock);
    if (cb_console_progress_active) {
        fputs("\r\033[K", stderr);
        fflush(stderr);
        cb_console_progress_active = 0;
    }
    pthread_mutex_unlock(&cb_console_lock);
}
