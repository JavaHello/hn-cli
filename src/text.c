#include "text.h"

#include <stdlib.h>
#include <string.h>

char *text_strip_html(const char *input) {
    if (input == NULL) {
        return strdup("");
    }

    size_t n = strlen(input);
    char *out = malloc(n + 1);
    if (out == NULL) {
        return NULL;
    }

    int in_tag = 0;
    size_t j = 0;
    for (size_t i = 0; i < n; i++) {
        char c = input[i];
        if (c == '<') {
            in_tag = 1;
            continue;
        }
        if (c == '>') {
            in_tag = 0;
            continue;
        }
        if (!in_tag) {
            out[j++] = c;
        }
    }
    out[j] = '\0';
    return out;
}

char *text_truncate_copy(const char *input, size_t max_len) {
    if (input == NULL) {
        return strdup("");
    }
    size_t n = strlen(input);
    if (n <= max_len) {
        return strdup(input);
    }
    char *out = malloc(max_len + 4);
    if (out == NULL) {
        return NULL;
    }
    memcpy(out, input, max_len);
    memcpy(out + max_len, "...", 4);
    return out;
}

char *text_join_two(const char *a, const char *b, const char *sep) {
    const char *sa = a ? a : "";
    const char *sb = b ? b : "";
    const char *ssep = sep ? sep : "";
    size_t len = strlen(sa) + strlen(sb) + strlen(ssep) + 1;
    char *out = malloc(len);
    if (out == NULL) {
        return NULL;
    }
    out[0] = '\0';
    strcat(out, sa);
    strcat(out, ssep);
    strcat(out, sb);
    return out;
}
