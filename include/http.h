#ifndef HTTP_H
#define HTTP_H

#include <stddef.h>

typedef int (*http_stream_callback)(const char *chunk, size_t len, void *user_data);

int http_get(const char *url, char **response, char **error_msg);
int http_post_json(const char *url, const char *json_body, const char *auth_bearer, char **response, char **error_msg);
int http_post_json_stream(const char *url, const char *json_body, const char *auth_bearer, http_stream_callback callback, void *user_data, char **error_msg);

#endif
