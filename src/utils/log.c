#define _GNU_SOURCE

#include "cws/log.h"

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define CWS_LOG_RING_SIZE 4096
#define CWS_LOG_MSG_MAX   512
#define CWS_LOG_FILE_MAX  64
#define CWS_LOG_FLUSH_MS  10

typedef struct {
    cws_log_level_t level;
    char            file[CWS_LOG_FILE_MAX];
    int             line;
    char            msg[CWS_LOG_MSG_MAX];
    struct timespec ts;
} cws_log_entry_t;

static struct {
    int                 initialized;
    cws_log_sink_fn     sink;
    void*               user;
    atomic_int          min_level;
    atomic_uint_fast64_t dropped;
    pthread_mutex_t     lock;
    pthread_cond_t      cond;
    pthread_t           flusher;
    cws_log_entry_t*   ring;
    size_t              head;
    size_t              tail;
    int                 running;
    int                 stop;
} g_log;

static const char* level_str(cws_log_level_t lvl) {
    switch (lvl) {
        case CWS_LOG_TRACE: return "TRACE";
        case CWS_LOG_DEBUG: return "DEBUG";
        case CWS_LOG_INFO:  return "INFO";
        case CWS_LOG_WARN:  return "WARN";
        case CWS_LOG_ERROR: return "ERROR";
        case CWS_LOG_FATAL: return "FATAL";
        default:            return "?";
    }
}

static const char* basename_of(const char* path) {
    const char* slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static void drain_ring(void) {
    while (g_log.tail != g_log.head) {
        cws_log_entry_t* e = &g_log.ring[g_log.tail];
        size_t msg_len = strnlen(e->msg, CWS_LOG_MSG_MAX);
        g_log.sink(e->level, e->file, e->line, e->msg, msg_len, g_log.user);
        g_log.tail = (g_log.tail + 1) % CWS_LOG_RING_SIZE;
    }
}

static void* flusher_main(void* arg) {
    (void)arg;
    pthread_mutex_lock(&g_log.lock);
    while (!g_log.stop) {
        struct timespec deadline;
        clock_gettime(CLOCK_REALTIME, &deadline);
        deadline.tv_nsec += (long)CWS_LOG_FLUSH_MS * 1000000L;
        if (deadline.tv_nsec >= 1000000000L) {
            deadline.tv_sec  += deadline.tv_nsec / 1000000000L;
            deadline.tv_nsec %= 1000000000L;
        }
        pthread_cond_timedwait(&g_log.cond, &g_log.lock, &deadline);
        drain_ring();
    }
    drain_ring();
    pthread_mutex_unlock(&g_log.lock);
    return NULL;
}

void cws_log_default_sink(cws_log_level_t level,
                          const char*     file,
                          int             line,
                          const char*     msg,
                          size_t          msg_len,
                          void*           user) {
    (void)msg_len;
    (void)user;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    struct timeval tv;
    gettimeofday(&tv, NULL);
    struct tm tm;
    localtime_r(&tv.tv_sec, &tm);
    char tbuf[16];
    strftime(tbuf, sizeof(tbuf), "%H:%M:%S", &tm);
    int ms = (int)(tv.tv_usec / 1000);
    fprintf(stderr, "[%s] %s.%03d %s:%d: %.*s\n",
            level_str(level), tbuf, ms, file, line,
            (int)msg_len, msg);
    (void)ts;
}

void cws_log_init(cws_log_level_t min_level, cws_log_sink_fn sink, void* user) {
    if (g_log.initialized) {
        return;
    }
    g_log.sink   = sink ? sink : cws_log_default_sink;
    g_log.user   = user;
    atomic_store(&g_log.min_level, (int)min_level);
    atomic_store(&g_log.dropped, 0);
    g_log.ring = (cws_log_entry_t*)calloc(CWS_LOG_RING_SIZE, sizeof(cws_log_entry_t));
    if (!g_log.ring) {
        return;
    }
    g_log.head = 0;
    g_log.tail = 0;
    g_log.stop = 0;
    pthread_mutex_init(&g_log.lock, NULL);
    pthread_cond_init(&g_log.cond, NULL);
    g_log.initialized = 1;
    g_log.running  = 1;
    if (pthread_create(&g_log.flusher, NULL, flusher_main, NULL) != 0) {
        g_log.running = 0;
    }
}

void cws_log_set_level(cws_log_level_t level) {
    atomic_store(&g_log.min_level, (int)level);
}

cws_log_level_t cws_log_get_level(void) {
    return (cws_log_level_t)atomic_load(&g_log.min_level);
}

void cws_log_write(cws_log_level_t level,
                   const char*     file,
                   int             line,
                   const char*     fmt, ...) {
    if (!g_log.initialized) {
        return;
    }
    int saved_errno = errno;
    int min = atomic_load(&g_log.min_level);
    if ((int)level < min) {
        errno = saved_errno;
        return;
    }
    char buf[CWS_LOG_MSG_MAX];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, CWS_LOG_MSG_MAX, fmt, ap);
    va_end(ap);
    if (n < 0) {
        buf[0] = '\0';
    } else if (n >= CWS_LOG_MSG_MAX) {
        buf[CWS_LOG_MSG_MAX - 1] = '\0';
    }
    const char* bn = basename_of(file);

    pthread_mutex_lock(&g_log.lock);
    size_t next = (g_log.head + 1) % CWS_LOG_RING_SIZE;
    if (next == g_log.tail) {
        pthread_mutex_unlock(&g_log.lock);
        atomic_fetch_add(&g_log.dropped, 1);
        errno = saved_errno;
        return;
    }
    cws_log_entry_t* e = &g_log.ring[g_log.head];
    e->level = level;
    strncpy(e->file, bn, CWS_LOG_FILE_MAX - 1);
    e->file[CWS_LOG_FILE_MAX - 1] = '\0';
    e->line = line;
    memcpy(e->msg, buf, CWS_LOG_MSG_MAX);
    e->msg[CWS_LOG_MSG_MAX - 1] = '\0';
    clock_gettime(CLOCK_MONOTONIC, &e->ts);
    g_log.head = next;
    size_t used = (g_log.head + CWS_LOG_RING_SIZE - g_log.tail) % CWS_LOG_RING_SIZE;
    int wake = (used >= CWS_LOG_RING_SIZE / 2);
    pthread_mutex_unlock(&g_log.lock);
    if (wake) {
        pthread_cond_signal(&g_log.cond);
    }
    errno = saved_errno;
}

void cws_log_flush(void) {
    if (!g_log.initialized) {
        return;
    }
    pthread_mutex_lock(&g_log.lock);
    pthread_cond_signal(&g_log.cond);
    pthread_mutex_unlock(&g_log.lock);
}

void cws_log_shutdown(void) {
    if (!g_log.initialized) {
        return;
    }
    pthread_mutex_lock(&g_log.lock);
    g_log.stop = 1;
    pthread_cond_signal(&g_log.cond);
    pthread_mutex_unlock(&g_log.lock);

    if (g_log.running) {
        pthread_join(g_log.flusher, NULL);
        g_log.running = 0;
    }

    pthread_mutex_lock(&g_log.lock);
    drain_ring();
    pthread_mutex_unlock(&g_log.lock);

    free(g_log.ring);
    g_log.ring = NULL;
    pthread_cond_destroy(&g_log.cond);
    pthread_mutex_destroy(&g_log.lock);
    atomic_store(&g_log.min_level, (int)CWS_LOG_LEVEL_COUNT);
    g_log.sink = NULL;
    g_log.user = NULL;
    g_log.head = 0;
    g_log.tail = 0;
    atomic_store(&g_log.dropped, 0);
    g_log.initialized = 0;
}
