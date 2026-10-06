#ifndef HN_API_H
#define HN_API_H

#include <stddef.h>

typedef struct {
    long id;
    char *title;
    char *by;
    int score;
    char *text;
    long *kids;
    size_t kids_count;
    int dead;
    int deleted;
} HNItem;

/* 取第 offset 条起的 count 条 story id。offset 越界时返回 0 条而不是报错。 */
int hn_fetch_story_ids(const char *type, size_t offset, size_t limit, long **ids, size_t *count, char **error_msg);

int hn_fetch_item(long id, HNItem *item, char **error_msg);

/* 并行抓取多个 item。items[i] 与 ok[i] 一一对应，单项失败只把 ok[i] 置 0。
 * 返回成功的条数。 */
size_t hn_fetch_items(const long *ids, size_t count, HNItem *items, int *ok);

void hn_item_free(HNItem *item);

int hn_collect_comment_texts(const long *kids, size_t kids_count, size_t max_comments, char **combined_text, char **error_msg);

#endif
