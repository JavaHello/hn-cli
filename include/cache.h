#ifndef CACHE_H
#define CACHE_H

typedef struct Cache Cache;

/* 打开本地缓存。失败时退化为空缓存，不会返回 NULL。 */
Cache *cache_open(void);

/* 立即原子落盘（内容有更新时才真正写）。长任务中途调用可以避免 Ctrl-C 丢掉已生成的内容。 */
void cache_flush(Cache *c);

/* dirty 时原子落盘并释放。 */
void cache_close(Cache *c);

/* 返回 1 表示命中（*out 为 strdup 出来的内容），0 表示未命中或已过期。 */
int cache_get_summary(Cache *c, long id, char **out);
int cache_get_detail(Cache *c, long id, char **out);

int cache_set_summary(Cache *c, long id, const char *summary);
int cache_set_detail(Cache *c, long id, const char *detail);

#endif
