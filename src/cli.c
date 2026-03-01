#include "cli.h"
#include "deepseek.h"
#include "hn_api.h"
#include "text.h"

#include <json-c/json.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define SUMMARY_TTL_SECONDS 86400

static const char *cache_file_path(void) {
    const char *p = getenv("HN_CLI_CACHE_FILE");
    if (p != NULL && p[0] != '\0') {
        return p;
    }
    return ".hn_cli_cache.json";
}

static struct json_object *cache_load(void) {
    const char *path = cache_file_path();
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return json_object_new_object();
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return json_object_new_object();
    }
    long sz = ftell(f);
    if (sz <= 0) {
        fclose(f);
        return json_object_new_object();
    }
    rewind(f);
    char *buf = malloc((size_t)sz + 1);
    if (buf == NULL) {
        fclose(f);
        return json_object_new_object();
    }
    size_t n = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[n] = '\0';
    struct json_object *obj = json_tokener_parse(buf);
    free(buf);
    if (obj == NULL || !json_object_is_type(obj, json_type_object)) {
        if (obj != NULL) {
            json_object_put(obj);
        }
        return json_object_new_object();
    }
    return obj;
}

static void cache_save(struct json_object *cache) {
    const char *path = cache_file_path();
    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        return;
    }
    const char *text = json_object_to_json_string_ext(cache, JSON_C_TO_STRING_PRETTY);
    fwrite(text, 1, strlen(text), f);
    fclose(f);
}

static int cache_get_summary(struct json_object *cache, long id, char **summary_out) {
    *summary_out = NULL;
    char key[32];
    snprintf(key, sizeof(key), "%ld", id);
    struct json_object *entry = NULL;
    if (!json_object_object_get_ex(cache, key, &entry) || !json_object_is_type(entry, json_type_object)) {
        return 0;
    }
    struct json_object *summary = NULL;
    struct json_object *updated = NULL;
    if (!json_object_object_get_ex(entry, "summary_zh", &summary) ||
        !json_object_object_get_ex(entry, "updated_at", &updated)) {
        return 0;
    }
    time_t now = time(NULL);
    long ts = json_object_get_int64(updated);
    if ((long)now - ts > SUMMARY_TTL_SECONDS) {
        return 0;
    }
    const char *s = json_object_get_string(summary);
    if (s == NULL || s[0] == '\0') {
        return 0;
    }
    *summary_out = strdup(s);
    return *summary_out ? 1 : 0;
}

static int cache_set_summary(struct json_object *cache, long id, const char *summary) {
    char key[32];
    snprintf(key, sizeof(key), "%ld", id);
    struct json_object *entry = json_object_new_object();
    if (entry == NULL) {
        return -1;
    }
    json_object_object_add(entry, "summary_zh", json_object_new_string(summary ? summary : ""));
    json_object_object_add(entry, "updated_at", json_object_new_int64((int64_t)time(NULL)));
    json_object_object_add(cache, key, entry);
    return 0;
}

static char *build_list_summary_source(const HNItem *item) {
    char id_part[64];
    snprintf(id_part, sizeof(id_part), "id:%ld", item->id);
    char *title = text_join_two("标题: ", item->title ? item->title : "", "");
    char *body_clean = text_strip_html(item->text ? item->text : "");
    char *body = text_join_two("正文: ", body_clean ? body_clean : "", "");
    char *head = text_join_two(id_part, title ? title : "", "\n");
    char *merged = text_join_two(head ? head : "", body ? body : "", "\n");
    free(title);
    free(body_clean);
    free(body);
    free(head);
    return merged;
}

static int print_list_with_ids(const long *ids, size_t count) {
    struct json_object *cache = cache_load();
    int dirty = 0;
    for (size_t i = 0; i < count; i++) {
        HNItem item;
        char *err = NULL;
        if (hn_fetch_item(ids[i], &item, &err) != 0) {
            fprintf(stderr, "warn: fetch item %ld failed: %s\n", ids[i], err ? err : "unknown");
            free(err);
            continue;
        }
        const char *title = item.title ? item.title : "(no title)";
        printf("[%zu] [%d] %s (id:%ld)\n", i + 1, item.score, title, item.id);

        char *summary = NULL;
        if (!cache_get_summary(cache, item.id, &summary)) {
            char *source = build_list_summary_source(&item);
            if (deepseek_summarize_one_line_zh(source ? source : title, &summary, &err) != 0) {
                free(err);
                summary = strdup("（生成失败）");
            } else {
                if (cache_set_summary(cache, item.id, summary) == 0) {
                    dirty = 1;
                }
            }
            free(source);
        }
        printf("总结: %s\n", summary ? summary : "（生成失败）");
        free(summary);
        hn_item_free(&item);
    }
    if (dirty) {
        cache_save(cache);
    }
    json_object_put(cache);
    return 0;
}

int cli_run_list(size_t limit) {
    long *ids = NULL;
    size_t count = 0;
    char *err = NULL;
    if (hn_fetch_top_ids(limit, &ids, &count, &err) != 0) {
        fprintf(stderr, "error: list fetch failed: %s\n", err ? err : "unknown");
        free(err);
        return 1;
    }
    int rc = print_list_with_ids(ids, count);
    free(ids);
    return rc;
}

static char *build_source_text(const HNItem *post) {
    char id_line[64];
    snprintf(id_line, sizeof(id_line), "Post ID: %ld", post->id);
    char *title = text_join_two("Title: ", post->title ? post->title : "", "");
    char *body_clean = text_strip_html(post->text ? post->text : "");
    char *body = text_join_two("Body: ", body_clean ? body_clean : "", "");
    char *head = text_join_two(id_line, title ? title : "", "\n");
    char *source = text_join_two(head ? head : "", body ? body : "", "\n");
    free(title);
    free(body_clean);
    free(body);
    free(head);
    return source;
}

int cli_run_open(long id) {
    HNItem post;
    char *err = NULL;
    if (hn_fetch_item(id, &post, &err) != 0) {
        fprintf(stderr, "error: fetch post failed: %s\n", err ? err : "unknown");
        free(err);
        return 1;
    }

    char *source = build_source_text(&post);
    char *comments = NULL;
    if (hn_collect_comment_texts(post.kids, post.kids_count, 20, &comments, &err) != 0) {
        fprintf(stderr, "warn: comment fetch failed: %s\n", err ? err : "unknown");
        free(err);
        comments = strdup("");
    }

    char *merged = text_join_two(source ? source : "", comments ? comments : "", "\nComments:\n");
    char *zh = NULL;
    if (deepseek_summarize_translate_zh(merged ? merged : "", &zh, &err) == 0) {
        printf("中文总结与翻译:\n%s\n", zh);
        free(zh);
    } else {
        char *raw = text_truncate_copy(merged ? merged : "", 400);
        fprintf(stderr, "翻译失败: %s\n", err ? err : "unknown");
        printf("原文片段:\n%s\n", raw ? raw : "");
        free(raw);
        free(err);
    }

    free(source);
    free(comments);
    free(merged);
    hn_item_free(&post);
    return 0;
}

int cli_run_open_index(size_t index, size_t limit) {
    if (index == 0) {
        fprintf(stderr, "error: invalid index\n");
        return 1;
    }

    long *ids = NULL;
    size_t count = 0;
    char *err = NULL;
    if (hn_fetch_top_ids(limit, &ids, &count, &err) != 0) {
        fprintf(stderr, "error: list fetch failed: %s\n", err ? err : "unknown");
        free(err);
        return 1;
    }
    if (index > count) {
        fprintf(stderr, "error: index out of range (max %zu)\n", count);
        free(ids);
        return 1;
    }
    long id = ids[index - 1];
    free(ids);
    return cli_run_open(id);
}

int cli_run_interactive(size_t limit) {
    long *ids = NULL;
    size_t count = 0;
    char *err = NULL;
    if (hn_fetch_top_ids(limit, &ids, &count, &err) != 0) {
        fprintf(stderr, "error: list fetch failed: %s\n", err ? err : "unknown");
        free(err);
        return 1;
    }

    print_list_with_ids(ids, count);
    printf("\n输入序号打开帖子 (q 退出): ");
    fflush(stdout);

    char line[64];
    if (fgets(line, sizeof(line), stdin) == NULL) {
        free(ids);
        return 0;
    }
    if (line[0] == 'q' || line[0] == 'Q') {
        free(ids);
        return 0;
    }
    long idx = strtol(line, NULL, 10);
    if (idx < 1 || (size_t)idx > count) {
        fprintf(stderr, "error: invalid index\n");
        free(ids);
        return 1;
    }

    long id = ids[idx - 1];
    free(ids);
    return cli_run_open(id);
}
