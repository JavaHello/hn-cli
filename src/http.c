#include "http.h"

#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char *data;
    size_t len;
} Buffer;

typedef struct {
    http_stream_callback callback;
    void *user_data;
} StreamContext;

static size_t write_cb(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t total = size * nmemb;
    Buffer *buf = (Buffer *)userp;
    char *p = realloc(buf->data, buf->len + total + 1);
    if (p == NULL) {
        return 0;
    }
    buf->data = p;
    memcpy(buf->data + buf->len, contents, total);
    buf->len += total;
    buf->data[buf->len] = '\0';
    return total;
}

static size_t write_stream_cb(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t total = size * nmemb;
    StreamContext *ctx = (StreamContext *)userp;
    if (ctx == NULL || ctx->callback == NULL) {
        return 0;
    }
    return ctx->callback((const char *)contents, total, ctx->user_data) ? total : 0;
}

static int do_request(const char *url, const char *method, const char *json_body, const char *auth_bearer, char **response, char **error_msg) {
    CURL *curl = curl_easy_init();
    if (curl == NULL) {
        *error_msg = strdup("curl init failed");
        return -1;
    }

    Buffer buf = {0};
    struct curl_slist *headers = NULL;

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 20L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "hn-cli/1.0");

    if (strcmp(method, "POST") == 0) {
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_body);
        headers = curl_slist_append(headers, "Content-Type: application/json");
        if (auth_bearer != NULL && auth_bearer[0] != '\0') {
            char auth[1024];
            snprintf(auth, sizeof(auth), "Authorization: Bearer %s", auth_bearer);
            headers = curl_slist_append(headers, auth);
        }
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    }

    CURLcode rc = curl_easy_perform(curl);
    if (rc != CURLE_OK) {
        *error_msg = strdup(curl_easy_strerror(rc));
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
        free(buf.data);
        return -1;
    }

    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    if (http_code < 200 || http_code >= 300) {
        char msg[128];
        snprintf(msg, sizeof(msg), "http status %ld", http_code);
        *error_msg = strdup(msg);
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
        free(buf.data);
        return -1;
    }

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    if (buf.data == NULL) {
        buf.data = strdup("");
    }
    *response = buf.data;
    return 0;
}

static int do_request_stream(const char *url, const char *method, const char *json_body, const char *auth_bearer, http_stream_callback callback, void *user_data, char **error_msg) {
    CURL *curl = curl_easy_init();
    if (curl == NULL) {
        *error_msg = strdup("curl init failed");
        return -1;
    }

    struct curl_slist *headers = NULL;
    StreamContext ctx = {
        .callback = callback,
        .user_data = user_data,
    };

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_stream_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ctx);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "hn-cli/1.0");

    if (strcmp(method, "POST") == 0) {
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_body);
        headers = curl_slist_append(headers, "Content-Type: application/json");
        if (auth_bearer != NULL && auth_bearer[0] != '\0') {
            char auth[1024];
            snprintf(auth, sizeof(auth), "Authorization: Bearer %s", auth_bearer);
            headers = curl_slist_append(headers, auth);
        }
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    }

    CURLcode rc = curl_easy_perform(curl);
    if (rc != CURLE_OK) {
        *error_msg = strdup(curl_easy_strerror(rc));
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
        return -1;
    }

    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    if (http_code < 200 || http_code >= 300) {
        char msg[128];
        snprintf(msg, sizeof(msg), "http status %ld", http_code);
        *error_msg = strdup(msg);
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
        return -1;
    }

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return 0;
}

int http_get(const char *url, char **response, char **error_msg) {
    return do_request(url, "GET", NULL, NULL, response, error_msg);
}

int http_post_json(const char *url, const char *json_body, const char *auth_bearer, char **response, char **error_msg) {
    return do_request(url, "POST", json_body, auth_bearer, response, error_msg);
}

int http_post_json_stream(const char *url, const char *json_body, const char *auth_bearer, http_stream_callback callback, void *user_data, char **error_msg) {
    return do_request_stream(url, "POST", json_body, auth_bearer, callback, user_data, error_msg);
}
