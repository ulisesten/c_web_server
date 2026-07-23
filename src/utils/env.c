#define _GNU_SOURCE

#include "cws/env.h"
#include "cws/errors.h"
#include "cws/log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#define CWS_ENV_BUCKETS 64

typedef struct cws_env_entry {
    char*                   key;
    char*                   value;
    struct cws_env_entry*   next;
} cws_env_entry_t;

struct cws_env {
    cws_env_entry_t* buckets[CWS_ENV_BUCKETS];
    size_t           size;
};

static unsigned long env_hash(const char* s) {
    unsigned long h = 5381;
    unsigned char c;
    while ((c = (unsigned char)*s++)) h = ((h << 5) + h) + (unsigned long)c;
    return h;
}

cws_env_t* cws_env_new(void) {
    cws_env_t* e = calloc(1, sizeof(*e));
    return e;
}

void cws_env_free(cws_env_t* env) {
    if (!env) return;
    for (size_t i = 0; i < CWS_ENV_BUCKETS; i++) {
        cws_env_entry_t* cur = env->buckets[i];
        while (cur) {
            cws_env_entry_t* next = cur->next;
            free(cur->key);
            free(cur->value);
            free(cur);
            cur = next;
        }
    }
    free(env);
}

static cws_env_entry_t* env_find(cws_env_t* env, const char* key) {
    unsigned long h = env_hash(key) % CWS_ENV_BUCKETS;
    for (cws_env_entry_t* e = env->buckets[h]; e; e = e->next) {
        if (strcmp(e->key, key) == 0) return e;
    }
    return NULL;
}

int cws_env_set(cws_env_t* env, const char* key, const char* value) {
    if (!env || !key || !value) return CWS_ERR_INVALID;
    cws_env_entry_t* e = env_find(env, key);
    if (e) {
        char* nv = strdup(value);
        if (!nv) return CWS_ERR_NOMEM;
        free(e->value);
        e->value = nv;
        return CWS_OK;
    }
    e = malloc(sizeof(*e));
    if (!e) return CWS_ERR_NOMEM;
    e->key = strdup(key);
    e->value = strdup(value);
    if (!e->key || !e->value) {
        free(e->key); free(e->value); free(e);
        return CWS_ERR_NOMEM;
    }
    unsigned long h = env_hash(key) % CWS_ENV_BUCKETS;
    e->next = env->buckets[h];
    env->buckets[h] = e;
    env->size++;
    return CWS_OK;
}

const char* cws_env_get(const cws_env_t* env, const char* key) {
    if (!env || !key) return NULL;
    cws_env_entry_t* e = env_find((cws_env_t*)env, key);
    return e ? e->value : NULL;
}

const char* cws_env_get_or(const cws_env_t* env, const char* key,
                           const char* default_value) {
    const char* v = cws_env_get(env, key);
    return v ? v : default_value;
}

static char* parse_value(const char* v) {
    while (*v == ' ' || *v == '\t') v++;
    size_t len = strlen(v);
    while (len > 0 && (v[len-1] == ' ' || v[len-1] == '\t')) len--;
    if (len == 0) return strdup("");
    char qc = v[0];
    if ((qc == '"' || qc == '\'') && len >= 2 && v[len-1] == qc) {
        char* out = malloc(len - 1);
        if (!out) return NULL;
        memcpy(out, v + 1, len - 2);
        out[len - 2] = '\0';
        return out;
    }
    char* out = malloc(len + 1);
    if (!out) return NULL;
    memcpy(out, v, len);
    out[len] = '\0';
    return out;
}

int cws_env_load_line(cws_env_t* env, const char* line) {
    if (!env || !line) return CWS_ERR_INVALID;
    const char* p = line;
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '\0' || *p == '\n' || *p == '#') return CWS_OK;

    if (strncmp(p, "export ", 7) == 0) p += 7;

    const char* eq = strchr(p, '=');
    if (!eq) {
        cws_log_warn("env: malformed line, no '=': %s", line);
        return CWS_ERR_INVALID;
    }
    size_t klen = (size_t)(eq - p);
    while (klen > 0 && (p[klen-1] == ' ' || p[klen-1] == '\t')) klen--;
    if (klen == 0) {
        cws_log_warn("env: empty key in line: %s", line);
        return CWS_ERR_INVALID;
    }
    char key[256];
    if (klen >= sizeof(key)) klen = sizeof(key) - 1;
    memcpy(key, p, klen);
    key[klen] = '\0';

    char* val = parse_value(eq + 1);
    if (!val) return CWS_ERR_NOMEM;

    int rc = cws_env_set(env, key, val);
    free(val);
    return rc;
}

int cws_env_load_file(cws_env_t* env, const char* path) {
    if (!env || !path) return CWS_ERR_INVALID;
    FILE* f = fopen(path, "r");
    if (!f) {
        cws_log_warn("env: cannot open %s", path);
        return CWS_ERR_IO;
    }
    char buf[1024];
    size_t lineno = 0;
    while (fgets(buf, sizeof(buf), f)) {
        lineno++;
        size_t n = strlen(buf);
        if (n > 0 && buf[n-1] == '\n') buf[n-1] = '\0';
        if (n > 1 && buf[n-2] == '\r') buf[n-2] = '\0';
        int rc = cws_env_load_line(env, buf);
        if (rc == CWS_ERR_INVALID) {
            cws_log_warn("env: %s:%zu skipped", path, lineno);
        }
    }
    fclose(f);
    cws_log_info("env: loaded %zu entries from %s", cws_env_size(env), path);
    return CWS_OK;
}

int cws_env_export_to_environ(const cws_env_t* env) {
    if (!env) return CWS_ERR_INVALID;
    for (size_t i = 0; i < CWS_ENV_BUCKETS; i++) {
        for (cws_env_entry_t* e = env->buckets[i]; e; e = e->next) {
            if (setenv(e->key, e->value, 1) != 0) {
                cws_log_warn("env: setenv(%s) failed", e->key);
            }
        }
    }
    return CWS_OK;
}

size_t cws_env_size(const cws_env_t* env) {
    return env ? env->size : 0;
}
