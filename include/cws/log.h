#ifndef CWS_LOG_H
#define CWS_LOG_H

#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>

typedef enum {
    CWS_LOG_TRACE = 0,
    CWS_LOG_DEBUG,
    CWS_LOG_INFO,
    CWS_LOG_WARN,
    CWS_LOG_ERROR,
    CWS_LOG_FATAL,
    CWS_LOG_LEVEL_COUNT
} cws_log_level_t;

typedef void (*cws_log_sink_fn)(cws_log_level_t level,
                                const char*     file,
                                int             line,
                                const char*     msg,
                                size_t          msg_len,
                                void*           user);

void        cws_log_init(cws_log_level_t min_level, cws_log_sink_fn sink, void* user);
void        cws_log_set_level(cws_log_level_t level);
cws_log_level_t cws_log_get_level(void);

void        cws_log_write(cws_log_level_t level,
                         const char*     file,
                         int             line,
                         const char*     fmt, ...)
                         __attribute__((format(printf, 4, 5)));

void        cws_log_flush(void);
void        cws_log_shutdown(void);

#define cws_log_trace(...) cws_log_write(CWS_LOG_TRACE, __FILE__, __LINE__, __VA_ARGS__)
#define cws_log_debug(...) cws_log_write(CWS_LOG_DEBUG, __FILE__, __LINE__, __VA_ARGS__)
#define cws_log_info(...)  cws_log_write(CWS_LOG_INFO,  __FILE__, __LINE__, __VA_ARGS__)
#define cws_log_warn(...)  cws_log_write(CWS_LOG_WARN,  __FILE__, __LINE__, __VA_ARGS__)
#define cws_log_error(...) cws_log_write(CWS_LOG_ERROR, __FILE__, __LINE__, __VA_ARGS__)
#define cws_log_fatal(...) cws_log_write(CWS_LOG_FATAL, __FILE__, __LINE__, __VA_ARGS__)

void cws_log_default_sink(cws_log_level_t level,
                          const char*     file,
                          int             line,
                          const char*     msg,
                          size_t          msg_len,
                          void*           user);

#endif
