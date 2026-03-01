#include "deepseek.h"
#include "http.h"

#include <json-c/json.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    long sz = ftell(f);
    if (sz < 0) {
        fclose(f);
        return NULL;
    }
    rewind(f);
    char *buf = malloc((size_t)sz + 1);
    if (buf == NULL) {
        fclose(f);
        return NULL;
    }
    size_t n = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[n] = '\0';
    return buf;
}

static int parse_deepseek_content(const char *json_text, char **result_text, char **error_msg) {
    struct json_object *obj = json_tokener_parse(json_text);
    if (obj == NULL) {
        *error_msg = strdup("invalid deepseek json");
        return -1;
    }
    struct json_object *choices = NULL;
    struct json_object *choice0 = NULL;
    struct json_object *message = NULL;
    struct json_object *content = NULL;
    int ok = json_object_object_get_ex(obj, "choices", &choices) &&
             json_object_is_type(choices, json_type_array) &&
             json_object_array_length(choices) > 0;
    if (ok) {
        choice0 = json_object_array_get_idx(choices, 0);
        ok = json_object_object_get_ex(choice0, "message", &message) &&
             json_object_object_get_ex(message, "content", &content);
    }
    if (!ok) {
        json_object_put(obj);
        *error_msg = strdup("deepseek response missing choices[0].message.content");
        return -1;
    }
    *result_text = strdup(json_object_get_string(content));
    json_object_put(obj);
    if (*result_text == NULL) {
        *error_msg = strdup("oom");
        return -1;
    }
    return 0;
}

static int deepseek_chat(const char *system_prompt, const char *user_prompt, char **result_text, char **error_msg) {
    *result_text = NULL;

    const char *explicit_mock = getenv("DEEPSEEK_MOCK_FILE");
    if (explicit_mock != NULL && explicit_mock[0] != '\0') {
        char *mock = read_file(explicit_mock);
        if (mock == NULL) {
            *error_msg = strdup("mock deepseek response not found");
            return -1;
        }
        int rc = parse_deepseek_content(mock, result_text, error_msg);
        free(mock);
        return rc;
    }

    const char *mock_dir = getenv("HN_CLI_MOCK_DIR");
    if (mock_dir != NULL && mock_dir[0] != '\0') {
        char path[1024];
        snprintf(path, sizeof(path), "%s/deepseek_response.json", mock_dir);
        char *mock = read_file(path);
        if (mock != NULL) {
            int rc = parse_deepseek_content(mock, result_text, error_msg);
            free(mock);
            return rc;
        }
    }

    const char *api_key = getenv("DEEPSEEK_API_KEY");
    if (api_key == NULL || api_key[0] == '\0') {
        *error_msg = strdup("DEEPSEEK_API_KEY is not set");
        return -1;
    }

    const char *base_url = getenv("DEEPSEEK_BASE_URL");
    if (base_url == NULL || base_url[0] == '\0') {
        base_url = "https://api.deepseek.com";
    }

    char url[512];
    snprintf(url, sizeof(url), "%s/chat/completions", base_url);

    struct json_object *root = json_object_new_object();
    struct json_object *msgs = json_object_new_array();
    struct json_object *sys = json_object_new_object();
    struct json_object *usr = json_object_new_object();

    json_object_object_add(root, "model", json_object_new_string("deepseek-chat"));
    json_object_object_add(sys, "role", json_object_new_string("system"));
    json_object_object_add(sys, "content", json_object_new_string(system_prompt));
    json_object_object_add(usr, "role", json_object_new_string("user"));
    json_object_object_add(usr, "content", json_object_new_string(user_prompt));

    json_object_array_add(msgs, sys);
    json_object_array_add(msgs, usr);
    json_object_object_add(root, "messages", msgs);
    json_object_object_add(root, "temperature", json_object_new_double(0.2));

    const char *body = json_object_to_json_string(root);
    char *resp = NULL;
    if (http_post_json(url, body, api_key, &resp, error_msg) != 0) {
        json_object_put(root);
        return -1;
    }
    json_object_put(root);
    int rc = parse_deepseek_content(resp, result_text, error_msg);
    free(resp);
    return rc;
}

int deepseek_summarize_translate_zh(const char *source_text, char **result_text, char **error_msg) {
    *result_text = NULL;
    const char *prefix = "请总结并翻译以下 Hacker News 帖子与评论内容到中文：\n\n";
    size_t prompt_len = strlen(prefix) + strlen(source_text) + 1;
    char *prompt = malloc(prompt_len);
    if (prompt == NULL) {
        *error_msg = strdup("oom");
        return -1;
    }
    strcpy(prompt, prefix);
    strcat(prompt, source_text);
    int rc = deepseek_chat(
        "你是技术内容助手。请将输入内容输出为中文，先给简要摘要，再给要点翻译。",
        prompt,
        result_text,
        error_msg);
    free(prompt);
    return rc;
}

int deepseek_summarize_one_line_zh(const char *source_text, char **result_text, char **error_msg) {
    const char *prefix = "请用一句中文总结以下 Hacker News 标题与内容，不超过30字：\n\n";
    size_t prompt_len = strlen(prefix) + strlen(source_text) + 1;
    char *prompt = malloc(prompt_len);
    if (prompt == NULL) {
        *error_msg = strdup("oom");
        return -1;
    }
    strcpy(prompt, prefix);
    strcat(prompt, source_text);
    int rc = deepseek_chat("你是中文技术摘要助手，只输出一句中文总结。", prompt, result_text, error_msg);
    free(prompt);
    return rc;
}
