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

int hn_fetch_top_ids(size_t limit, long **ids, size_t *count, char **error_msg);
int hn_fetch_item(long id, HNItem *item, char **error_msg);
void hn_item_free(HNItem *item);

int hn_collect_comment_texts(const long *kids, size_t kids_count, size_t max_comments, char **combined_text, char **error_msg);

#endif
