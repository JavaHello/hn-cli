#include "cache.h"

#include <json-c/json.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define DEFAULT_TTL_SECONDS 86400
/* 完整总结翻译是一次昂贵的调用（还要抓评论），缓存留久一点。 */
#define DEFAULT_DETAIL_TTL_SECONDS 604800
#define LEGACY_CACHE_FILE ".hn_cli_cache.json"

/*
 * 缓存文件格式（id 为 key）：
 *   { "1001": { "summary_zh": "...", "summary_at": 1791294621,
 *               "detail_zh":  "...", "detail_at":  1791294622 } }
 * 旧格式只有 summary_zh + updated_at，读取时会回退到 updated_at。
 */
struct Cache {
    struct json_object *root;
    char *path;
    int dirty;
    long ttl;
    long detail_ttl;
};

static char *dup_join(const char *a, const char *b) {
    size_t la = strlen(a);
    size_t lb = strlen(b);
    char *out = malloc(la + lb + 1);
    if (out == NULL) {
        return NULL;
    }
    memcpy(out, a, la);
    memcpy(out + la, b, lb + 1);
    return out;
}

/* HN_CLI_CACHE_TTL 同时覆盖列表总结和完整总结的 TTL；0 表示永不过期。 */
static int env_ttl(long *out) {
    const char *env = getenv("HN_CLI_CACHE_TTL");
    if (env == NULL || env[0] == '\0') {
        return 0;
    }
    char *end = NULL;
    long v = strtol(env, &end, 10);
    if (end == NULL || *end != '\0' || v < 0) {
        return 0;
    }
    *out = v;
    return 1;
}

static char *default_cache_path(void) {
    const char *xdg = getenv("XDG_CACHE_HOME");
    if (xdg != NULL && xdg[0] != '\0') {
        return dup_join(xdg, "/hn-cli/cache.json");
    }
    const char *home = getenv("HOME");
    if (home != NULL && home[0] != '\0') {
        return dup_join(home, "/.cache/hn-cli/cache.json");
    }
    return strdup(LEGACY_CACHE_FILE);
}

/* 逐级创建 path 的父目录，已存在则忽略。 */
static void ensure_parent_dir(const char *path) {
    char *copy = strdup(path);
    if (copy == NULL) {
        return;
    }
    char *slash = strrchr(copy, '/');
    if (slash == NULL || slash == copy) {
        free(copy);
        return;
    }
    *slash = '\0';
    for (char *p = copy + 1; *p != '\0'; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(copy, 0755);
            *p = '/';
        }
    }
    mkdir(copy, 0755);
    free(copy);
}

static char *read_whole_file(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    long sz = ftell(f);
    if (sz <= 0) {
        fclose(f);
        return NULL;
    }
    rewind(f);
    char *buf = malloc((size_t)sz + 1);
    if (buf == NULL) {
        fclose(f);
        return NULL;
    }
    size_t n = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[n] = '\0';
    if (out_len != NULL) {
        *out_len = n;
    }
    return buf;
}

static struct json_object *load_json_file(const char *path) {
    char *buf = read_whole_file(path, NULL);
    if (buf == NULL) {
        return NULL;
    }
    struct json_object *obj = json_tokener_parse(buf);
    free(buf);
    if (obj == NULL || !json_object_is_type(obj, json_type_object)) {
        if (obj != NULL) {
            json_object_put(obj);
        }
        return NULL;
    }
    return obj;
}

Cache *cache_open(void) {
    Cache *c = calloc(1, sizeof(*c));
    if (c == NULL) {
        return NULL;
    }
    long override_ttl = 0;
    if (env_ttl(&override_ttl)) {
        c->ttl = override_ttl;
        c->detail_ttl = override_ttl;
    } else {
        c->ttl = DEFAULT_TTL_SECONDS;
        c->detail_ttl = DEFAULT_DETAIL_TTL_SECONDS;
    }

    const char *env = getenv("HN_CLI_CACHE_FILE");
    int explicit_path = env != NULL && env[0] != '\0';
    c->path = explicit_path ? strdup(env) : default_cache_path();
    if (c->path == NULL) {
        free(c);
        return NULL;
    }

    /* 未显式指定路径时，把工作目录里的旧缓存迁移到新的 XDG 位置（只复制，不删原文件）。 */
    const char *load_path = c->path;
    int migrating = 0;
    if (!explicit_path && access(c->path, F_OK) != 0 && access(LEGACY_CACHE_FILE, R_OK) == 0) {
        load_path = LEGACY_CACHE_FILE;
        migrating = 1;
    }

    c->root = load_json_file(load_path);
    if (c->root == NULL) {
        c->root = json_object_new_object();
        if (c->root == NULL) {
            free(c->path);
            free(c);
            return NULL;
        }
    }
    c->dirty = migrating;
    return c;
}

/* 丢弃 summary/detail 都已过期的条目，避免缓存文件无限增长。 */
static void cache_prune(Cache *c) {
    long max_ttl = c->ttl > c->detail_ttl ? c->ttl : c->detail_ttl;
    if (max_ttl <= 0) {
        return;
    }
    long now = (long)time(NULL);
    struct json_object *doomed = json_object_new_array();
    if (doomed == NULL) {
        return;
    }

    json_object_object_foreach(c->root, key, entry) {
        if (entry == NULL || !json_object_is_type(entry, json_type_object)) {
            json_object_array_add(doomed, json_object_new_string(key));
            continue;
        }
        long newest = 0;
        struct json_object *stamp = NULL;
        static const char *stamp_keys[] = {"summary_at", "detail_at", "updated_at"};
        for (size_t i = 0; i < sizeof(stamp_keys) / sizeof(stamp_keys[0]); i++) {
            if (json_object_object_get_ex(entry, stamp_keys[i], &stamp)) {
                long v = (long)json_object_get_int64(stamp);
                if (v > newest) {
                    newest = v;
                }
            }
        }
        if (newest <= 0 || newest > now || now - newest > max_ttl) {
            json_object_array_add(doomed, json_object_new_string(key));
        }
    }

    size_t n = json_object_array_length(doomed);
    for (size_t i = 0; i < n; i++) {
        struct json_object *k = json_object_array_get_idx(doomed, (int)i);
        const char *key = json_object_get_string(k);
        if (key != NULL) {
            json_object_object_del(c->root, key);
        }
    }
    json_object_put(doomed);
}

static int cache_save(Cache *c) {
    if (!c->dirty) {
        return 0;
    }
    cache_prune(c);

    size_t path_len = strlen(c->path);
    char *tmp_path = malloc(path_len + 32);
    if (tmp_path == NULL) {
        return -1;
    }
    snprintf(tmp_path, path_len + 32, "%s.tmp.%ld", c->path, (long)getpid());

    ensure_parent_dir(c->path);

    const char *text = json_object_to_json_string_ext(c->root, JSON_C_TO_STRING_PRETTY);
    size_t len = strlen(text);
    FILE *f = fopen(tmp_path, "wb");
    if (f == NULL) {
        free(tmp_path);
        return -1;
    }
    if (fwrite(text, 1, len, f) != len) {
        fclose(f);
        unlink(tmp_path);
        free(tmp_path);
        return -1;
    }
    /* 先落盘再 rename，否则掉电时可能留下一个已改名但内容为空的文件。 */
    int write_failed = fflush(f) != 0 || fsync(fileno(f)) != 0;
    if (fclose(f) != 0) {
        write_failed = 1;
    }
    if (write_failed) {
        unlink(tmp_path);
        free(tmp_path);
        return -1;
    }
    if (rename(tmp_path, c->path) != 0) {
        unlink(tmp_path);
        free(tmp_path);
        return -1;
    }
    free(tmp_path);
    c->dirty = 0;
    return 0;
}

void cache_flush(Cache *c) {
    if (c == NULL) {
        return;
    }
    cache_save(c);
}

void cache_close(Cache *c) {
    if (c == NULL) {
        return;
    }
    cache_save(c);
    json_object_put(c->root);
    free(c->path);
    free(c);
}

static int is_detail_key(const char *text_key) {
    return strcmp(text_key, "detail_zh") == 0;
}

static const char *entry_time_key(const char *text_key) {
    return is_detail_key(text_key) ? "detail_at" : "summary_at";
}

static long cache_text_ttl(const Cache *c, const char *text_key) {
    return is_detail_key(text_key) ? c->detail_ttl : c->ttl;
}

static int entry_get_text(struct json_object *entry, const char *text_key, long ttl, char **out) {
    struct json_object *text = NULL;
    if (!json_object_object_get_ex(entry, text_key, &text)) {
        return 0;
    }

    long stamp = 0;
    struct json_object *stamp_obj = NULL;
    if (json_object_object_get_ex(entry, entry_time_key(text_key), &stamp_obj) ||
        json_object_object_get_ex(entry, "updated_at", &stamp_obj)) {
        stamp = (long)json_object_get_int64(stamp_obj);
    }
    if (ttl > 0) {
        long now = (long)time(NULL);
        /* 时间戳缺失（<= 0）或来自未来（时钟回拨）都按过期处理，否则会永远命中。 */
        if (stamp <= 0 || stamp > now || now - stamp > ttl) {
            return 0;
        }
    }

    const char *s = json_object_get_string(text);
    if (s == NULL || s[0] == '\0') {
        return 0;
    }
    *out = strdup(s);
    return *out != NULL ? 1 : 0;
}

static int cache_get_text(Cache *c, long id, const char *text_key, char **out) {
    *out = NULL;
    if (c == NULL || c->root == NULL) {
        return 0;
    }
    char key[32];
    snprintf(key, sizeof(key), "%ld", id);

    struct json_object *entry = NULL;
    if (!json_object_object_get_ex(c->root, key, &entry) ||
        !json_object_is_type(entry, json_type_object)) {
        return 0;
    }
    return entry_get_text(entry, text_key, cache_text_ttl(c, text_key), out);
}

static int cache_set_text(Cache *c, long id, const char *text_key, const char *value) {
    if (c == NULL || c->root == NULL || value == NULL || value[0] == '\0') {
        return -1;
    }
    char key[32];
    snprintf(key, sizeof(key), "%ld", id);

    struct json_object *entry = NULL;
    if (!json_object_object_get_ex(c->root, key, &entry) ||
        !json_object_is_type(entry, json_type_object)) {
        entry = json_object_new_object();
        if (entry == NULL) {
            return -1;
        }
        json_object_object_add(c->root, key, entry);
    }

    json_object_object_add(entry, text_key, json_object_new_string(value));
    json_object_object_add(entry, entry_time_key(text_key), json_object_new_int64((int64_t)time(NULL)));
    c->dirty = 1;
    return 0;
}

int cache_get_summary(Cache *c, long id, char **out) {
    return cache_get_text(c, id, "summary_zh", out);
}

int cache_get_detail(Cache *c, long id, char **out) {
    return cache_get_text(c, id, "detail_zh", out);
}

int cache_set_summary(Cache *c, long id, const char *summary) {
    return cache_set_text(c, id, "summary_zh", summary);
}

int cache_set_detail(Cache *c, long id, const char *detail) {
    return cache_set_text(c, id, "detail_zh", detail);
}
