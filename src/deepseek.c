#include "deepseek.h"
#include "http.h"

#include <json-c/json.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char *pending;
    size_t pending_len;
    char *result;
    size_t result_len;
    char *error_msg;
    deepseek_output_callback output_callback;
    void *output_user_data;
} DeepseekStreamState;

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

static int append_bytes(char **buf, size_t *buf_len, const char *data, size_t data_len) {
    char *next = realloc(*buf, *buf_len + data_len + 1);
    if (next == NULL) {
        return -1;
    }
    *buf = next;
    memcpy(*buf + *buf_len, data, data_len);
    *buf_len += data_len;
    (*buf)[*buf_len] = '\0';
    return 0;
}

static int emit_output(DeepseekStreamState *state, const char *text) {
    if (state == NULL || state->output_callback == NULL || text == NULL || text[0] == '\0') {
        return 0;
    }
    if (!state->output_callback(text, state->output_user_data)) {
        free(state->error_msg);
        state->error_msg = strdup("deepseek output callback failed");
        return -1;
    }
    return 0;
}

static int parse_deepseek_content(const char *json_text, DeepseekStreamState *state, char **result_text, char **error_msg) {
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
    if (emit_output(state, *result_text) != 0) {
        *error_msg = state->error_msg;
        state->error_msg = NULL;
        free(*result_text);
        *result_text = NULL;
        return -1;
    }
    return 0;
}

static int append_deepseek_chunk_text(struct json_object *chunk_obj, DeepseekStreamState *state) {
    struct json_object *choices = NULL;
    struct json_object *choice0 = NULL;
    struct json_object *delta = NULL;
    struct json_object *message = NULL;
    struct json_object *content = NULL;
    int ok = json_object_object_get_ex(chunk_obj, "choices", &choices) &&
             json_object_is_type(choices, json_type_array) &&
             json_object_array_length(choices) > 0;
    if (!ok) {
        return 0;
    }

    choice0 = json_object_array_get_idx(choices, 0);
    if (choice0 == NULL) {
        return 0;
    }

    if (json_object_object_get_ex(choice0, "delta", &delta) &&
        json_object_object_get_ex(delta, "content", &content)) {
        const char *piece = json_object_get_string(content);
        if (piece == NULL) {
            return 0;
        }
        if (append_bytes(&state->result, &state->result_len, piece, strlen(piece)) != 0) {
            return -1;
        }
        return emit_output(state, piece);
    }

    if (json_object_object_get_ex(choice0, "message", &message) &&
        json_object_object_get_ex(message, "content", &content)) {
        const char *piece = json_object_get_string(content);
        if (piece == NULL) {
            return 0;
        }
        if (append_bytes(&state->result, &state->result_len, piece, strlen(piece)) != 0) {
            return -1;
        }
        return emit_output(state, piece);
    }

    return 0;
}

static int process_stream_event(const char *payload, DeepseekStreamState *state) {
    while (*payload == ' ' || *payload == '\t') {
        payload++;
    }
    if (strcmp(payload, "[DONE]") == 0) {
        return 0;
    }

    struct json_object *obj = json_tokener_parse(payload);
    if (obj == NULL) {
        free(state->error_msg);
        state->error_msg = strdup("invalid deepseek stream chunk");
        return -1;
    }

    int rc = append_deepseek_chunk_text(obj, state);
    json_object_put(obj);
    if (rc != 0) {
        free(state->error_msg);
        state->error_msg = strdup("oom");
        return -1;
    }
    return 0;
}

static int process_stream_line(const char *line, size_t line_len, DeepseekStreamState *state) {
    size_t prefix_len = 5;
    if (line_len < prefix_len || strncmp(line, "data:", prefix_len) != 0) {
        return 0;
    }

    const char *payload = line + prefix_len;
    size_t payload_len = line_len - prefix_len;
    if (payload_len > 0 && *payload == ' ') {
        payload++;
        payload_len--;
    }

    char *copy = malloc(payload_len + 1);
    if (copy == NULL) {
        free(state->error_msg);
        state->error_msg = strdup("oom");
        return -1;
    }
    memcpy(copy, payload, payload_len);
    copy[payload_len] = '\0';
    int rc = process_stream_event(copy, state);
    free(copy);
    return rc;
}

static int process_stream_buffer(DeepseekStreamState *state, int flush_all) {
    size_t start = 0;
    size_t i = 0;

    while (i < state->pending_len) {
        if (state->pending[i] != '\n') {
            i++;
            continue;
        }

        size_t line_len = i - start;
        if (line_len > 0 && state->pending[start + line_len - 1] == '\r') {
            line_len--;
        }
        if (process_stream_line(state->pending + start, line_len, state) != 0) {
            return -1;
        }
        start = i + 1;
        i++;
    }

    if (flush_all && start < state->pending_len) {
        size_t line_len = state->pending_len - start;
        if (line_len > 0 && state->pending[start + line_len - 1] == '\r') {
            line_len--;
        }
        int rc = process_stream_line(state->pending + start, line_len, state);
        if (rc != 0) {
            return -1;
        }
        start = state->pending_len;
    }

    if (start == 0) {
        return 0;
    }
    if (start >= state->pending_len) {
        free(state->pending);
        state->pending = NULL;
        state->pending_len = 0;
        return 0;
    }

    size_t remaining = state->pending_len - start;
    memmove(state->pending, state->pending + start, remaining);
    state->pending_len = remaining;
    state->pending[remaining] = '\0';
    return 0;
}

static int deepseek_stream_callback(const char *chunk, size_t len, void *user_data) {
    DeepseekStreamState *state = (DeepseekStreamState *)user_data;
    if (append_bytes(&state->pending, &state->pending_len, chunk, len) != 0) {
        free(state->error_msg);
        state->error_msg = strdup("oom");
        return 0;
    }
    return process_stream_buffer(state, 0) == 0;
}

static int parse_deepseek_payload(const char *payload, deepseek_output_callback output_callback, void *user_data, char **result_text, char **error_msg) {
    while (*payload == ' ' || *payload == '\n' || *payload == '\r' || *payload == '\t') {
        payload++;
    }
    if (strncmp(payload, "data:", 5) != 0) {
        DeepseekStreamState state = {
            .output_callback = output_callback,
            .output_user_data = user_data,
        };
        return parse_deepseek_content(payload, &state, result_text, error_msg);
    }

    DeepseekStreamState state = {
        .output_callback = output_callback,
        .output_user_data = user_data,
    };
    if (append_bytes(&state.pending, &state.pending_len, payload, strlen(payload)) != 0) {
        *error_msg = strdup("oom");
        return -1;
    }
    if (process_stream_buffer(&state, 1) != 0) {
        *error_msg = state.error_msg ? state.error_msg : strdup("invalid deepseek stream chunk");
        free(state.pending);
        free(state.result);
        return -1;
    }
    free(state.pending);
    if (state.result == NULL) {
        *error_msg = strdup("deepseek stream response missing content");
        return -1;
    }
    *result_text = state.result;
    free(state.error_msg);
    return 0;
}

static int deepseek_chat(const char *system_prompt, const char *user_prompt, deepseek_output_callback output_callback, void *user_data, char **result_text, char **error_msg) {
    *result_text = NULL;

    const char *explicit_mock = getenv("DEEPSEEK_MOCK_FILE");
    if (explicit_mock != NULL && explicit_mock[0] != '\0') {
        char *mock = read_file(explicit_mock);
        if (mock == NULL) {
            *error_msg = strdup("mock deepseek response not found");
            return -1;
        }
        int rc = parse_deepseek_payload(mock, output_callback, user_data, result_text, error_msg);
        free(mock);
        return rc;
    }

    const char *mock_dir = getenv("HN_CLI_MOCK_DIR");
    if (mock_dir != NULL && mock_dir[0] != '\0') {
        char path[1024];
        snprintf(path, sizeof(path), "%s/deepseek_response.json", mock_dir);
        char *mock = read_file(path);
        if (mock != NULL) {
            int rc = parse_deepseek_payload(mock, output_callback, user_data, result_text, error_msg);
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
    json_object_object_add(root, "stream", json_object_new_boolean(1));

    const char *body = json_object_to_json_string(root);
    DeepseekStreamState state = {
        .output_callback = output_callback,
        .output_user_data = user_data,
    };
    if (http_post_json_stream(url, body, api_key, deepseek_stream_callback, &state, error_msg) != 0) {
        json_object_put(root);
        free(state.pending);
        free(state.result);
        free(state.error_msg);
        return -1;
    }
    json_object_put(root);
    if (process_stream_buffer(&state, 1) != 0) {
        *error_msg = state.error_msg ? state.error_msg : strdup("invalid deepseek stream chunk");
        free(state.pending);
        free(state.result);
        return -1;
    }
    free(state.pending);
    free(state.error_msg);
    if (state.result == NULL) {
        *error_msg = strdup("deepseek stream response missing content");
        return -1;
    }
    *result_text = state.result;
    return 0;
}

int deepseek_summarize_translate_zh(const char *source_text, char **result_text, char **error_msg) {
    return deepseek_summarize_translate_zh_stream(source_text, NULL, NULL, result_text, error_msg);
}

int deepseek_summarize_translate_zh_stream(const char *source_text, deepseek_output_callback output_callback, void *user_data, char **result_text, char **error_msg) {
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
        output_callback,
        user_data,
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
    int rc = deepseek_chat("你是中文技术摘要助手，只输出一句中文总结。", prompt, NULL, NULL, result_text, error_msg);
    free(prompt);
    return rc;
}
