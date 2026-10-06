#include "cli.h"
#include "cache.h"
#include "deepseek.h"
#include "hn_api.h"
#include "text.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_COMMENTS 20

static const char *type_label(const char *type) {
    if (type == NULL || strcmp(type, "top") == 0) {
        return "头条";
    }
    if (strcmp(type, "past") == 0) {
        return "最新";
    }
    if (strcmp(type, "ask") == 0) {
        return "Ask HN";
    }
    if (strcmp(type, "show") == 0) {
        return "Show HN";
    }
    return type;
}

static int fetch_page(const char *type, size_t page, size_t page_size, long **ids, size_t *count, char **error_msg) {
    size_t offset = (page - 1) * page_size;
    return hn_fetch_story_ids(type, offset, page_size, ids, count, error_msg);
}

/* 拼给模型看的帖子正文。 */
static char *build_source_text(const HNItem *item) {
    char id_line[64];
    snprintf(id_line, sizeof(id_line), "Post ID: %ld", item->id);
    char *body_clean = text_strip_html(item->text ? item->text : "");
    char *head = text_join_two(id_line, item->title ? item->title : "", "\nTitle: ");
    char *body = text_join_two("Body: ", body_clean ? body_clean : "", "");
    char *source = text_join_two(head ? head : "", body ? body : "", "\n");
    free(body_clean);
    free(body);
    free(head);
    return source;
}

/* 打印一页的条目，base_index 是本页第一条的全局序号（从 0 开始）。返回成功打印的条数。 */
static size_t print_items(const long *ids, size_t count, size_t base_index) {
    if (count == 0) {
        return 0;
    }
    HNItem *items = calloc(count, sizeof(HNItem));
    int *ok = calloc(count, sizeof(int));
    if (items == NULL || ok == NULL) {
        fprintf(stderr, "error: oom\n");
        free(items);
        free(ok);
        return 0;
    }

    hn_fetch_items(ids, count, items, ok);

    Cache *cache = cache_open();
    size_t printed = 0;
    size_t since_flush = 0;
    for (size_t i = 0; i < count; i++) {
        if (!ok[i]) {
            fprintf(stderr, "warn: fetch item %ld failed\n", ids[i]);
            continue;
        }
        printed++;
        printf("[%zu] [%d] %s (id:%ld)\n", base_index + i + 1, items[i].score,
               items[i].title ? items[i].title : "(no title)", items[i].id);

        char *summary = NULL;
        if (!cache_get_summary(cache, items[i].id, &summary)) {
            char *source = build_source_text(&items[i]);
            char *err = NULL;
            if (deepseek_summarize_one_line_zh(source ? source : "", &summary, &err) != 0) {
                free(err);
                summary = strdup("（生成失败）");
            } else {
                cache_set_summary(cache, items[i].id, summary);
            }
            free(source);
        }
        printf("总结: %s\n", summary ? summary : "（生成失败）");
        free(summary);

        /* 长列表跑到一半被 Ctrl-C 时，已经生成的内容不至于全丢。 */
        if (++since_flush >= 5) {
            cache_flush(cache);
            since_flush = 0;
        }
    }
    cache_close(cache);

    for (size_t i = 0; i < count; i++) {
        hn_item_free(&items[i]);
    }
    free(items);
    free(ok);
    return printed;
}

int cli_run_list(const char *type, size_t page, size_t page_size) {
    long *ids = NULL;
    size_t count = 0;
    char *err = NULL;
    if (fetch_page(type, page, page_size, &ids, &count, &err) != 0) {
        fprintf(stderr, "error: list fetch failed: %s\n", err ? err : "unknown");
        free(err);
        return 1;
    }

    printf("=== %s 第 %zu 页 ===\n", type_label(type), page);
    if (count == 0) {
        printf("没有更多了。\n");
        free(ids);
        return 0;
    }

    size_t printed = print_items(ids, count, (page - 1) * page_size);
    if (printed == 0) {
        fprintf(stderr, "error: 这一页的帖子全部获取失败（网络或 API 异常）\n");
        free(ids);
        return 1;
    }
    if (page > 1) {
        printf("上一页: -p %zu\n", page - 1);
    }
    if (count == page_size) {
        printf("下一页: -p %zu\n", page + 1);
    }
    free(ids);
    return 0;
}

typedef struct {
    FILE *out;
    char *buf;
    size_t len;
} StreamSink;

static int stream_sink_write(const char *chunk, void *user_data) {
    StreamSink *sink = (StreamSink *)user_data;
    if (fputs(chunk, sink->out) == EOF || fflush(sink->out) != 0) {
        return 0;
    }
    size_t n = strlen(chunk);
    char *p = realloc(sink->buf, sink->len + n + 1);
    if (p == NULL) {
        return 0;
    }
    sink->buf = p;
    memcpy(sink->buf + sink->len, chunk, n);
    sink->len += n;
    sink->buf[sink->len] = '\0';
    return 1;
}

static void print_cached_detail(const char *detail) {
    printf("中文总结与翻译: [本地缓存]\n%s", detail);
    size_t n = strlen(detail);
    if (n == 0 || detail[n - 1] != '\n') {
        putchar('\n');
    }
}

int cli_run_open(long id) {
    return cli_run_open_ex(id, 0);
}

int cli_run_open_ex(long id, int refresh) {
    Cache *cache = cache_open();
    char *cached = NULL;
    if (!refresh && cache_get_detail(cache, id, &cached)) {
        print_cached_detail(cached);
        free(cached);
        cache_close(cache);
        return 0;
    }

    HNItem post;
    char *err = NULL;
    if (hn_fetch_item(id, &post, &err) != 0) {
        fprintf(stderr, "error: fetch post failed: %s\n", err ? err : "unknown");
        free(err);
        cache_close(cache);
        return 1;
    }

    char *source = build_source_text(&post);
    char *comments = NULL;
    if (hn_collect_comment_texts(post.kids, post.kids_count, MAX_COMMENTS, &comments, &err) != 0) {
        fprintf(stderr, "warn: comment fetch failed: %s\n", err ? err : "unknown");
        free(err);
        comments = strdup("");
    }

    char *merged = text_join_two(source ? source : "", comments ? comments : "", "\nComments:\n");
    StreamSink sink = {.out = stdout};
    char *zh = NULL;
    printf("中文总结与翻译:\n");

    int rc = 0;
    if (deepseek_summarize_translate_zh_stream(merged ? merged : "", stream_sink_write, &sink, &zh, &err) == 0) {
        if (sink.len == 0 || sink.buf[sink.len - 1] != '\n') {
            putchar('\n');
        }
        /* 只在完整拿到结果后才落缓存，避免存下被中断的半截内容。 */
        if (sink.len > 0) {
            cache_set_detail(cache, id, sink.buf);
        }
        free(zh);
    } else {
        char *raw = text_truncate_copy(merged ? merged : "", 400);
        fprintf(stderr, "翻译失败: %s\n", err ? err : "unknown");
        printf("原文片段:\n%s\n", raw ? raw : "");
        free(raw);
        free(err);
        rc = 1;
    }

    free(sink.buf);
    free(source);
    free(comments);
    free(merged);
    hn_item_free(&post);
    cache_close(cache);
    return rc;
}

int cli_run_open_index(const char *type, size_t index) {
    if (index == 0) {
        fprintf(stderr, "error: invalid index\n");
        return 1;
    }

    long *ids = NULL;
    size_t count = 0;
    char *err = NULL;
    if (hn_fetch_story_ids(type, index - 1, 1, &ids, &count, &err) != 0) {
        fprintf(stderr, "error: list fetch failed: %s\n", err ? err : "unknown");
        free(err);
        return 1;
    }
    if (count == 0) {
        fprintf(stderr, "error: index out of range\n");
        free(ids);
        return 1;
    }
    long id = ids[0];
    free(ids);
    return cli_run_open(id);
}

static void print_page(const char *type, size_t page, size_t page_size, const long *ids, size_t count) {
    printf("\n=== %s 第 %zu 页 ===\n", type_label(type), page);
    print_items(ids, count, (page - 1) * page_size);
}

int cli_run_interactive(const char *type, size_t page_size) {
    size_t page = 1;
    long *ids = NULL;
    size_t count = 0;
    int need_fetch = 1;
    long last_opened = 0;

    for (;;) {
        if (need_fetch) {
            free(ids);
            ids = NULL;
            count = 0;
            char *err = NULL;
            if (fetch_page(type, page, page_size, &ids, &count, &err) != 0) {
                fprintf(stderr, "error: list fetch failed: %s\n", err ? err : "unknown");
                free(err);
                return 1;
            }
            need_fetch = 0;
        }

        if (count == 0) {
            printf("\n=== %s 第 %zu 页 ===\n没有更多了。\n", type_label(type), page);
            if (page > 1) {
                page--;
                need_fetch = 1;
                continue;
            }
            free(ids);
            return 0;
        }

        print_page(type, page, page_size, ids, count);
        printf("\n[序号]打开帖子  [n]下一页  [p]上一页  [r]重看上一个（忽略缓存）  [q]退出\n> ");
        fflush(stdout);

        char line[256];
        if (fgets(line, sizeof(line), stdin) == NULL) {
            putchar('\n');
            free(ids);
            return 0;
        }
        /* 超长输入会残留在 stdin 里被当成下一条命令，这里主动丢弃。 */
        if (strchr(line, '\n') == NULL && !feof(stdin)) {
            int ch;
            while ((ch = getchar()) != '\n' && ch != EOF) {
            }
        }

        char *p = line;
        while (*p == ' ' || *p == '\t') {
            p++;
        }
        if (*p == '\n' || *p == '\0') {
            continue;
        }
        if (*p == 'q' || *p == 'Q') {
            free(ids);
            return 0;
        }
        if (*p == 'n' || *p == 'N') {
            page++;
            need_fetch = 1;
            continue;
        }
        if (*p == 'p' || *p == 'P') {
            if (page > 1) {
                page--;
                need_fetch = 1;
            } else {
                printf("已经是第一页。\n");
            }
            continue;
        }
        if (*p == 'r' || *p == 'R') {
            if (last_opened == 0) {
                printf("还没有打开过帖子。\n");
                continue;
            }
            printf("\n--- 重新获取 id:%ld（忽略缓存）---\n", last_opened);
            cli_run_open_ex(last_opened, 1);
            continue;
        }

        char *end = NULL;
        long idx = strtol(p, &end, 10);
        if (end == p) {
            printf("无法识别的输入。\n");
            continue;
        }

        size_t base = (page - 1) * page_size;
        size_t local = 0;
        if (idx >= (long)base + 1 && idx <= (long)(base + count)) {
            local = (size_t)idx - base;
        } else if (idx >= 1 && (size_t)idx <= count) {
            local = (size_t)idx;
        } else {
            printf("序号超出范围。\n");
            continue;
        }
        last_opened = ids[local - 1];
        printf("\n--- 打开 [%zu] (id:%ld) ---\n", base + local, last_opened);
        cli_run_open(last_opened);
    }
}
