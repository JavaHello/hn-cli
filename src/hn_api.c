#include "hn_api.h"
#include "http.h"
#include "text.h"

#include <json-c/json.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HN_BASE "https://hacker-news.firebaseio.com/v0"
#define HN_ALGOLIA_PAST_URL "https://hn.algolia.com/api/v1/search_by_date?tags=story"

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

int hn_fetch_story_ids(const char *type, size_t limit, long **ids, size_t *count, char **error_msg) {
    const char *endpoint = NULL;
    const char *mock_name = NULL;
    if (resolve_story_endpoint(type, &endpoint, &mock_name) != 0) {
        *error_msg = strdup("invalid story type");
        return -1;
    }

    char url[256];
    if (strcmp(type, "past") == 0) {
        snprintf(url, sizeof(url), "%s", endpoint);
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
    if (strcmp(type, "past") == 0) {
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
    size_t n = total < limit ? total : limit;
    long *out = calloc(n, sizeof(long));
    if (out == NULL) {
        json_object_put(arr);
        *error_msg = strdup("oom");
        return -1;
    }

    for (size_t i = 0; i < n; i++) {
        struct json_object *v = json_object_array_get_idx(arr, (int)i);
        if (strcmp(type, "past") == 0) {
            struct json_object *id_obj = NULL;
            if (v != NULL && json_object_is_type(v, json_type_object) &&
                json_object_object_get_ex(v, "objectID", &id_obj)) {
                out[i] = strtol(json_object_get_string(id_obj), NULL, 10);
            } else {
                out[i] = 0;
            }
        } else {
            out[i] = (long)json_object_get_int64(v);
        }
    }
    json_object_put(root);
    *ids = out;
    *count = n;
    return 0;
}

int hn_fetch_top_ids(size_t limit, long **ids, size_t *count, char **error_msg) {
    return hn_fetch_story_ids("top", limit, ids, count, error_msg);
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
    if (obj == NULL || !json_object_is_type(obj, json_type_object)) {
        *error_msg = strdup("invalid item json");
        if (obj) {
            json_object_put(obj);
        }
        return -1;
    }

    struct json_object *v = NULL;
    if (json_object_object_get_ex(obj, "id", &v)) {
        item->id = (long)json_object_get_int64(v);
    } else {
        item->id = id;
    }
    if (json_object_object_get_ex(obj, "title", &v)) {
        item->title = strdup(json_object_get_string(v));
    }
    if (json_object_object_get_ex(obj, "by", &v)) {
        item->by = strdup(json_object_get_string(v));
    }
    if (json_object_object_get_ex(obj, "score", &v)) {
        item->score = json_object_get_int(v);
    }
    if (json_object_object_get_ex(obj, "text", &v)) {
        item->text = strdup(json_object_get_string(v));
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
            item->kids = calloc(n, sizeof(long));
            if (item->kids == NULL) {
                json_object_put(obj);
                *error_msg = strdup("oom");
                hn_item_free(item);
                return -1;
            }
            item->kids_count = n;
            for (size_t i = 0; i < n; i++) {
                struct json_object *kid = json_object_array_get_idx(v, (int)i);
                item->kids[i] = (long)json_object_get_int64(kid);
            }
        }
    }

    json_object_put(obj);
    return 0;
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
    size_t taken = 0;

    for (size_t i = 0; i < kids_count && taken < max_comments; i++) {
        HNItem comment;
        char *err = NULL;
        if (hn_fetch_item(kids[i], &comment, &err) != 0) {
            free(err);
            continue;
        }

        if (!comment.dead && !comment.deleted && comment.text != NULL) {
            char *clean = text_strip_html(comment.text);
            if (clean != NULL && clean[0] != '\0') {
                if (append_text(combined_text, clean) != 0) {
                    free(clean);
                    hn_item_free(&comment);
                    return -1;
                }
                taken++;
            }
            free(clean);
        }
        hn_item_free(&comment);
    }

    if (*combined_text == NULL) {
        *combined_text = strdup("");
    }
    return *combined_text ? 0 : -1;
}
