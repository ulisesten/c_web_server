#ifndef CWS_ENV_H
#define CWS_ENV_H

#include <stddef.h>

/*
 * Minimal .env loader.
 *
 * File format:
 *   KEY=VALUE
 *   KEY="quoted value with spaces"
 *   KEY='single quoted'
 *   # comment line
 *   export KEY=VALUE           (leading "export " is stripped)
 *
 * Empty lines and lines starting with '#' are ignored. Inline comments
 * are NOT supported; quote your values if you need a literal '#'.
 *
 * The loader stores entries in an internal hash map. Values are copied
 * and owned by the loader. Lookups return a pointer valid until
 * cws_env_free() is called.
 *
 * Loaded variables do NOT pollute getenv()/setenv() by default; call
 * cws_env_export_to_environ() if you want them visible to libc.
 */

typedef struct cws_env cws_env_t;

cws_env_t* cws_env_new(void);
void       cws_env_free(cws_env_t* env);

/*
 * Parse a .env file and merge entries into the map. Returns CWS_OK
 * or CWS_ERR_IO if the file cannot be opened. Parse errors are
 * logged via cws_log_warn and skipped (lenient mode).
 */
int  cws_env_load_file(cws_env_t* env, const char* path);

/*
 * Parse a single "KEY=VALUE" line and insert it. Returns CWS_OK or
 * CWS_ERR_INVALID on malformed input.
 */
int  cws_env_load_line(cws_env_t* env, const char* line);

/*
 * Get a value by key, or NULL if not present.
 */
const char* cws_env_get(const cws_env_t* env, const char* key);

/*
 * Get a value by key with a fallback default if missing.
 */
const char* cws_env_get_or(const cws_env_t* env, const char* key,
                          const char* default_value);

/*
 * Set/override a value programmatically.
 */
int  cws_env_set(cws_env_t* env, const char* key, const char* value);

/*
 * Copy every entry to the libc environment via setenv(). Useful when
 * you want downstream C library code (e.g., TLS) to see the values.
 */
int  cws_env_export_to_environ(const cws_env_t* env);

/*
 * Number of loaded entries.
 */
size_t cws_env_size(const cws_env_t* env);

#endif
