#ifndef HTTP_H
#define HTTP_H

int http_get(const char *url, char **response, char **error_msg);
int http_post_json(const char *url, const char *json_body, const char *auth_bearer, char **response, char **error_msg);

#endif
