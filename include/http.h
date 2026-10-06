#ifndef HTTP_H
#define HTTP_H

#include <stddef.h>

typedef int (*http_stream_callback)(const char *chunk, size_t len, void *user_data);

/* 进程启动时调用一次即可，可重复调用。 */
void http_global_init(void);

typedef struct {
    char *body;  /* 成功时为响应体（malloc），失败为 NULL */
    char *error; /* 失败时为错误信息（malloc），成功为 NULL */
} HttpResult;

int http_get(const char *url, char **response, char **error_msg);
int http_post_json(const char *url, const char *json_body, const char *auth_bearer, char **response, char **error_msg);
int http_post_json_stream(const char *url, const char *json_body, const char *auth_bearer, http_stream_callback callback, void *user_data, char **error_msg);

/* 并行 GET。单条失败只写进对应 results[i].error，不影响其余请求。
 * 返回 0 表示已逐个回填结果，-1 表示整体初始化失败。 */
int http_get_many(const char *const *urls, size_t count, HttpResult *results, size_t max_parallel);

void http_result_free(HttpResult *result);

#endif
