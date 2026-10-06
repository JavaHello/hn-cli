#include "http.h"

#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CONNECT_TIMEOUT_SECONDS 10L
#define REQUEST_TIMEOUT_SECONDS 20L
/* 流式响应没有总时长上限，只在长时间收不到数据时才中断。 */
#define STREAM_STALL_SECONDS 120L

typedef struct {
    char *data;
    size_t len;
} Buffer;

typedef struct {
    http_stream_callback callback;
    void *user_data;
} StreamContext;

typedef struct {
    CURL *easy;
    Buffer buf;
    size_t index;
} MultiSlot;

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

static struct curl_slist *build_headers(const char *method, const char *auth_bearer) {
    if (strcmp(method, "POST") != 0) {
        return NULL;
    }
    struct curl_slist *headers = curl_slist_append(NULL, "Content-Type: application/json");
    if (auth_bearer != NULL && auth_bearer[0] != '\0') {
        char auth[1024];
        snprintf(auth, sizeof(auth), "Authorization: Bearer %s", auth_bearer);
        headers = curl_slist_append(headers, auth);
    }
    return headers;
}

static void apply_common_opts(CURL *curl, const char *url, const char *method, const char *json_body, struct curl_slist *headers) {
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, CONNECT_TIMEOUT_SECONDS);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "hn-cli/1.0");

    if (strcmp(method, "POST") == 0) {
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_body);
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    }
}

static char *http_status_error(CURL *curl) {
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    if (http_code >= 200 && http_code < 300) {
        return NULL;
    }
    char msg[128];
    snprintf(msg, sizeof(msg), "http status %ld", http_code);
    return strdup(msg);
}

static int do_request(const char *url, const char *method, const char *json_body, const char *auth_bearer, char **response, char **error_msg) {
    *error_msg = NULL;
    *response = NULL;

    CURL *curl = curl_easy_init();
    if (curl == NULL) {
        *error_msg = strdup("curl init failed");
        return -1;
    }

    Buffer buf = {0};
    struct curl_slist *headers = build_headers(method, auth_bearer);

    apply_common_opts(curl, url, method, json_body, headers);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, REQUEST_TIMEOUT_SECONDS);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);

    CURLcode rc = curl_easy_perform(curl);
    if (rc != CURLE_OK) {
        *error_msg = strdup(curl_easy_strerror(rc));
    } else {
        *error_msg = http_status_error(curl);
    }
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (*error_msg != NULL) {
        free(buf.data);
        return -1;
    }
    *response = buf.data != NULL ? buf.data : strdup("");
    return 0;
}

static int do_request_stream(const char *url, const char *method, const char *json_body, const char *auth_bearer, http_stream_callback callback, void *user_data, char **error_msg) {
    *error_msg = NULL;

    CURL *curl = curl_easy_init();
    if (curl == NULL) {
        *error_msg = strdup("curl init failed");
        return -1;
    }

    struct curl_slist *headers = build_headers(method, auth_bearer);
    StreamContext ctx = {
        .callback = callback,
        .user_data = user_data,
    };

    apply_common_opts(curl, url, method, json_body, headers);
    /* 不用 CURLOPT_TIMEOUT：长翻译会被总时长腰斩。改为「连续 STALL 秒没有数据」才中断。 */
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, STREAM_STALL_SECONDS);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_stream_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ctx);

    CURLcode rc = curl_easy_perform(curl);
    if (rc != CURLE_OK) {
        *error_msg = strdup(curl_easy_strerror(rc));
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
        return -1;
    }

    char *status_error = http_status_error(curl);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    if (status_error != NULL) {
        *error_msg = status_error;
        return -1;
    }
    return 0;
}

static void multi_slot_finish(CURLM *multi, MultiSlot *slot, CURLcode result, HttpResult *out) {
    if (result != CURLE_OK) {
        out->error = strdup(curl_easy_strerror(result));
    } else {
        out->error = http_status_error(slot->easy);
        if (out->error == NULL) {
            out->body = slot->buf.data != NULL ? slot->buf.data : strdup("");
            slot->buf.data = NULL; /* 所有权转移给调用方 */
        }
    }
    free(slot->buf.data);
    slot->buf.data = NULL;
    slot->buf.len = 0;
    curl_multi_remove_handle(multi, slot->easy);
    curl_easy_cleanup(slot->easy);
    slot->easy = NULL;
}

int http_get_many(const char *const *urls, size_t count, HttpResult *results, size_t max_parallel) {
    if (count == 0) {
        return 0;
    }
    if (max_parallel == 0) {
        max_parallel = 1;
    }

    for (size_t i = 0; i < count; i++) {
        results[i].body = NULL;
        results[i].error = NULL;
    }

    CURLM *multi = curl_multi_init();
    if (multi == NULL) {
        return -1;
    }
    MultiSlot *slots = calloc(count, sizeof(*slots));
    if (slots == NULL) {
        curl_multi_cleanup(multi);
        return -1;
    }

    size_t next = 0;
    size_t in_flight = 0;
    size_t finished = 0;
    int init_failed = 0;

    while (finished < count) {
        while (next < count && in_flight < max_parallel) {
            size_t i = next;
            if (urls[i] == NULL) {
                results[i].error = strdup("empty url");
                next++;
                finished++;
                continue;
            }
            CURL *easy = curl_easy_init();
            if (easy == NULL) {
                results[i].error = strdup("curl init failed");
                next++;
                finished++;
                continue;
            }
            slots[i].easy = easy;
            slots[i].index = i;
            curl_easy_setopt(easy, CURLOPT_PRIVATE, &slots[i]);
            apply_common_opts(easy, urls[i], "GET", NULL, NULL);
            curl_easy_setopt(easy, CURLOPT_TIMEOUT, REQUEST_TIMEOUT_SECONDS);
            curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, write_cb);
            curl_easy_setopt(easy, CURLOPT_WRITEDATA, &slots[i].buf);
            if (curl_multi_add_handle(multi, easy) != CURLM_OK) {
                results[i].error = strdup("curl multi add failed");
                curl_easy_cleanup(easy);
                slots[i].easy = NULL;
                next++;
                finished++;
                continue;
            }
            next++;
            in_flight++;
        }

        int running = 0;
        if (curl_multi_perform(multi, &running) != CURLM_OK) {
            init_failed = 1;
            break;
        }

        CURLMsg *msg = NULL;
        int queued = 0;
        while ((msg = curl_multi_info_read(multi, &queued)) != NULL) {
            if (msg->msg != CURLMSG_DONE) {
                continue;
            }
            char *priv = NULL;
            curl_easy_getinfo(msg->easy_handle, CURLINFO_PRIVATE, &priv);
            MultiSlot *slot = (MultiSlot *)priv;
            if (slot == NULL || slot->easy != msg->easy_handle) {
                continue;
            }
            multi_slot_finish(multi, slot, msg->data.result, &results[slot->index]);
            in_flight--;
            finished++;
        }

        if (in_flight > 0) {
            curl_multi_poll(multi, NULL, 0, 100, NULL);
        }
    }

    if (init_failed) {
        for (size_t i = 0; i < count; i++) {
            if (slots[i].easy != NULL) {
                curl_multi_remove_handle(multi, slots[i].easy);
                curl_easy_cleanup(slots[i].easy);
                slots[i].easy = NULL;
            }
            if (results[i].error == NULL && results[i].body == NULL) {
                results[i].error = strdup("curl multi perform failed");
            }
        }
    }

    for (size_t i = 0; i < count; i++) {
        free(slots[i].buf.data);
    }
    free(slots);
    curl_multi_cleanup(multi);
    return init_failed ? -1 : 0;
}

void http_global_init(void) {
    curl_global_init(CURL_GLOBAL_DEFAULT);
}

void http_result_free(HttpResult *result) {
    if (result == NULL) {
        return;
    }
    free(result->body);
    free(result->error);
    result->body = NULL;
    result->error = NULL;
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
