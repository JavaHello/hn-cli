#include "hn_api.h"
#include "http.h"
#include "text.h"

#include <json-c/json.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HN_BASE "https://hacker-news.firebaseio.com/v0"
#define HN_ALGOLIA_PAST_URL "https://hn.algolia.com/api/v1/search_by_date?tags=story"
/* Algolia 该端点最多返回 1000 条，hitsPerPage 也有 1000 的上限。 */
#define HN_ALGOLIA_MAX_HITS 1000
#define MAX_PARALLEL_FETCH 8

static int mock_mode(void) {
    const char *mock_dir = getenv("HN_CLI_MOCK_DIR");
    return mock_dir != NULL && mock_dir[0] != '\0';
}

static char *read_mock_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    long sz = ftell(f);
    if (sz < 0) {
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
    return buf;
}

static char *fetch_url_or_mock(const char *url, const char *mock_name, char **error_msg) {
    const char *mock_dir = getenv("HN_CLI_MOCK_DIR");
    if (mock_dir != NULL && mock_dir[0] != '\0') {
        char path[1024];
        snprintf(path, sizeof(path), "%s/%s", mock_dir, mock_name);
        char *data = read_mock_file(path);
        if (data == NULL) {
            *error_msg = strdup("mock file not found");
            return NULL;
        }
        return data;
    }

    char *resp = NULL;
    if (http_get(url, &resp, error_msg) != 0) {
        return NULL;
    }
    return resp;
}

void hn_item_free(HNItem *item) {
    if (item == NULL) {
        return;
    }
    free(item->title);
    free(item->by);
    free(item->text);
    free(item->kids);
    memset(item, 0, sizeof(*item));
}

static int resolve_story_endpoint(const char *type, const char **endpoint, const char **mock_name) {
    if (type == NULL || strcmp(type, "top") == 0) {
        *endpoint = "topstories.json";
        *mock_name = "topstories.json";
        return 0;
    }
    if (strcmp(type, "past") == 0) {
        *endpoint = HN_ALGOLIA_PAST_URL;
        *mock_name = "algolia_paststories.json";
        return 0;
    }
    if (strcmp(type, "ask") == 0) {
        *endpoint = "askstories.json";
        *mock_name = "askstories.json";
        return 0;
    }
    if (strcmp(type, "show") == 0) {
        *endpoint = "showstories.json";
        *mock_name = "showstories.json";
        return 0;
    }
    return -1;
}

static char *dup_json_string(struct json_object *obj, const char *key) {
    struct json_object *v = NULL;
    if (!json_object_object_get_ex(obj, key, &v)) {
        return NULL;
    }
    const char *s = json_object_get_string(v);
    return s != NULL ? strdup(s) : NULL;
}

static int parse_item_json(struct json_object *obj, long fallback_id, HNItem *item) {
    memset(item, 0, sizeof(*item));
    if (obj == NULL || !json_object_is_type(obj, json_type_object)) {
        return -1;
    }

    struct json_object *v = NULL;
    item->id = json_object_object_get_ex(obj, "id", &v) ? (long)json_object_get_int64(v) : fallback_id;
    item->title = dup_json_string(obj, "title");
    item->by = dup_json_string(obj, "by");
    item->text = dup_json_string(obj, "text");
    if (json_object_object_get_ex(obj, "score", &v)) {
        item->score = json_object_get_int(v);
    }
    if (json_object_object_get_ex(obj, "dead", &v)) {
        item->dead = json_object_get_boolean(v);
    }
    if (json_object_object_get_ex(obj, "deleted", &v)) {
        item->deleted = json_object_get_boolean(v);
    }

    if (json_object_object_get_ex(obj, "kids", &v) && json_object_is_type(v, json_type_array)) {
        size_t n = (size_t)json_object_array_length(v);
        if (n > 0) {
            item->kids = malloc(n * sizeof(long));
            if (item->kids == NULL) {
                hn_item_free(item);
                return -1;
            }
            item->kids_count = n;
            for (size_t i = 0; i < n; i++) {
                struct json_object *kid = json_object_array_get_idx(v, (int)i);
                item->kids[i] = kid != NULL ? (long)json_object_get_int64(kid) : 0;
            }
        }
    }
    return 0;
}

int hn_fetch_story_ids(const char *type, size_t offset, size_t limit, long **ids, size_t *count, char **error_msg) {
    *ids = NULL;
    *count = 0;

    const char *endpoint = NULL;
    const char *mock_name = NULL;
    if (resolve_story_endpoint(type, &endpoint, &mock_name) != 0) {
        *error_msg = strdup("invalid story type");
        return -1;
    }
    if (limit == 0) {
        return 0;
    }

    int is_past = strcmp(type, "past") == 0;
    /* Algolia 只允许翻到前 1000 条，超出时它会返回空 hits，必须和「真的没有了」区分开。 */
    if (is_past && !mock_mode() && offset >= HN_ALGOLIA_MAX_HITS) {
        *error_msg = strdup("past 列表最多只能翻到前 1000 条");
        return -1;
    }
    char url[512];
    /* local_offset 是「相对于服务端返回的这一页」还要跳过多少条。 */
    size_t local_offset = offset;

    if (is_past) {
        if (mock_mode()) {
            snprintf(url, sizeof(url), "%s", endpoint);
        } else {
            size_t page_size = limit > HN_ALGOLIA_MAX_HITS ? HN_ALGOLIA_MAX_HITS : limit;
            size_t page = offset / page_size;
            local_offset = offset % page_size;
            snprintf(url, sizeof(url), "%s&page=%zu&hitsPerPage=%zu", endpoint, page, page_size);
        }
    } else {
        snprintf(url, sizeof(url), HN_BASE "/%s", endpoint);
    }

    char *payload = fetch_url_or_mock(url, mock_name, error_msg);
    if (payload == NULL) {
        return -1;
    }

    struct json_object *root = json_tokener_parse(payload);
    free(payload);
    if (root == NULL) {
        *error_msg = strdup("invalid story ids json");
        return -1;
    }

    struct json_object *arr = NULL;
    if (is_past) {
        if (!json_object_is_type(root, json_type_object) ||
            !json_object_object_get_ex(root, "hits", &arr) ||
            !json_object_is_type(arr, json_type_array)) {
            *error_msg = strdup("invalid past stories json");
            json_object_put(root);
            return -1;
        }
    } else {
        if (!json_object_is_type(root, json_type_array)) {
            *error_msg = strdup("invalid story ids json");
            json_object_put(root);
            return -1;
        }
        arr = root;
    }

    size_t total = (size_t)json_object_array_length(arr);
    if (local_offset >= total) {
        json_object_put(root);
        return 0;
    }
    size_t available = total - local_offset;
    size_t n = available < limit ? available : limit;
    if (n == 0) {
        json_object_put(root);
        return 0;
    }

    long *out = malloc(n * sizeof(long));
    if (out == NULL) {
        json_object_put(root);
        *error_msg = strdup("oom");
        return -1;
    }

    for (size_t i = 0; i < n; i++) {
        struct json_object *v = json_object_array_get_idx(arr, (int)(local_offset + i));
        if (is_past) {
            struct json_object *id_obj = NULL;
            const char *s = NULL;
            if (v != NULL && json_object_is_type(v, json_type_object) &&
                json_object_object_get_ex(v, "objectID", &id_obj)) {
                s = json_object_get_string(id_obj);
            }
            out[i] = s != NULL ? strtol(s, NULL, 10) : 0;
        } else {
            out[i] = v != NULL ? (long)json_object_get_int64(v) : 0;
        }
    }
    json_object_put(root);

    *ids = out;
    *count = n;
    return 0;
}

int hn_fetch_item(long id, HNItem *item, char **error_msg) {
    memset(item, 0, sizeof(*item));

    char url[256];
    char mock_name[256];
    snprintf(url, sizeof(url), HN_BASE "/item/%ld.json", id);
    snprintf(mock_name, sizeof(mock_name), "item_%ld.json", id);
    char *payload = fetch_url_or_mock(url, mock_name, error_msg);
    if (payload == NULL) {
        return -1;
    }

    struct json_object *obj = json_tokener_parse(payload);
    free(payload);
    if (parse_item_json(obj, id, item) != 0) {
        *error_msg = strdup("invalid item json");
        if (obj != NULL) {
            json_object_put(obj);
        }
        return -1;
    }
    json_object_put(obj);
    return 0;
}

size_t hn_fetch_items(const long *ids, size_t count, HNItem *items, int *ok) {
    for (size_t i = 0; i < count; i++) {
        memset(&items[i], 0, sizeof(HNItem));
        ok[i] = 0;
    }
    if (count == 0) {
        return 0;
    }

    /* mock 模式读的是本地文件，顺序处理即可。 */
    if (mock_mode()) {
        size_t good = 0;
        for (size_t i = 0; i < count; i++) {
            char *err = NULL;
            if (hn_fetch_item(ids[i], &items[i], &err) == 0) {
                ok[i] = 1;
                good++;
            }
            free(err);
        }
        return good;
    }

    enum { URL_CAP = 128 };
    char **urls = calloc(count, sizeof(char *));
    HttpResult *results = calloc(count, sizeof(HttpResult));
    if (urls == NULL || results == NULL) {
        free(urls);
        free(results);
        return 0;
    }

    for (size_t i = 0; i < count; i++) {
        urls[i] = malloc(URL_CAP);
        if (urls[i] != NULL) {
            snprintf(urls[i], URL_CAP, HN_BASE "/item/%ld.json", ids[i]);
        }
        /* malloc 失败也不中止：http_get_many 会把这一条标记为失败，其余照常抓取。 */
    }

    size_t good = 0;
    if (http_get_many((const char *const *)urls, count, results, MAX_PARALLEL_FETCH) == 0) {
        for (size_t i = 0; i < count; i++) {
            if (results[i].body == NULL) {
                continue;
            }
            struct json_object *obj = json_tokener_parse(results[i].body);
            if (parse_item_json(obj, ids[i], &items[i]) == 0) {
                ok[i] = 1;
                good++;
            }
            if (obj != NULL) {
                json_object_put(obj);
            }
        }
    }

    for (size_t i = 0; i < count; i++) {
        free(urls[i]);
        http_result_free(&results[i]);
    }
    free(urls);
    free(results);
    return good;
}

static int append_text(char **dst, const char *chunk) {
    if (chunk == NULL || chunk[0] == '\0') {
        return 0;
    }
    if (*dst == NULL) {
        *dst = strdup(chunk);
        return *dst ? 0 : -1;
    }
    size_t old_len = strlen(*dst);
    size_t add_len = strlen(chunk);
    char *p = realloc(*dst, old_len + add_len + 2);
    if (p == NULL) {
        return -1;
    }
    *dst = p;
    (*dst)[old_len] = '\n';
    memcpy(*dst + old_len + 1, chunk, add_len + 1);
    return 0;
}

int hn_collect_comment_texts(const long *kids, size_t kids_count, size_t max_comments, char **combined_text, char **error_msg) {
    (void)error_msg;
    *combined_text = NULL;
    if (max_comments == 0 || kids_count == 0) {
        *combined_text = strdup("");
        return *combined_text ? 0 : -1;
    }

    HNItem *items = calloc(max_comments, sizeof(HNItem));
    int *ok = calloc(max_comments, sizeof(int));
    if (items == NULL || ok == NULL) {
        free(items);
        free(ok);
        return -1;
    }

    size_t taken = 0;
    size_t next = 0;
    size_t scanned = 0;
    /* 大量评论被删/被折叠时，限制最多扫多少条，避免一直发请求。 */
    size_t scan_limit = max_comments * 5;

    while (next < kids_count && taken < max_comments && scanned < scan_limit) {
        size_t batch = max_comments - taken;
        if (batch > kids_count - next) {
            batch = kids_count - next;
        }
        if (batch > scan_limit - scanned) {
            batch = scan_limit - scanned;
        }

        hn_fetch_items(kids + next, batch, items, ok);
        for (size_t i = 0; i < batch; i++) {
            if (ok[i] && !items[i].dead && !items[i].deleted && items[i].text != NULL) {
                char *clean = text_strip_html(items[i].text);
                if (clean != NULL && clean[0] != '\0') {
                    if (append_text(combined_text, clean) != 0) {
                        free(clean);
                        for (size_t k = i; k < batch; k++) {
                            hn_item_free(&items[k]);
                        }
                        free(items);
                        free(ok);
                        free(*combined_text);
                        *combined_text = NULL;
                        return -1;
                    }
                    taken++;
                }
                free(clean);
            }
            /* 必须逐条释放：下一批会复用 items 数组，hn_fetch_items 会 memset 掉这些指针。 */
            hn_item_free(&items[i]);
        }
        next += batch;
        scanned += batch;
    }

    free(items);
    free(ok);

    if (*combined_text == NULL) {
        *combined_text = strdup("");
    }
    return *combined_text ? 0 : -1;
}
