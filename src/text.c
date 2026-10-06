#include "text.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    const char *name;
    const char *utf8;
} HtmlEntity;

/* Hacker News 正文里常见实体的子集，够用即可；未收录的实体保持原样输出。 */
static const HtmlEntity ENTITIES[] = {
    {"amp", "&"},     {"lt", "<"},      {"gt", ">"},      {"quot", "\""},   {"apos", "'"},
    {"nbsp", " "},    {"hellip", "…"},  {"mdash", "—"},   {"ndash", "–"},   {"lsquo", "‘"},
    {"rsquo", "’"},   {"ldquo", "“"},   {"rdquo", "”"},   {"middot", "·"},  {"times", "×"},
    {"copy", "©"},    {"reg", "®"},     {"trade", "™"},   {"laquo", "«"},   {"raquo", "»"},
    {"deg", "°"},     {"euro", "€"},    {"pound", "£"},   {"yen", "¥"},     {"sect", "§"},
    {"para", "¶"},    {"bull", "•"},    {"dagger", "†"},  {"permil", "‰"},  {"prime", "′"},
};

/* 这些标签本身代表分段，剥掉标签时要补一个换行，否则段落会粘成一行。 */
static const char *const BLOCK_TAGS[] = {
    "p", "br", "div", "li", "ul", "ol", "tr", "td", "th", "table",
    "blockquote", "pre", "h1", "h2", "h3", "h4", "h5", "h6", "hr", "section",
};

static size_t utf8_encode(unsigned long cp, char *out) {
    if (cp < 0x80) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    if (cp <= 0x10FFFF) {
        out[0] = (char)(0xF0 | (cp >> 18));
        out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[3] = (char)(0x80 | (cp & 0x3F));
        return 4;
    }
    return 0;
}

/* 解析 &...; 实体，成功返回消耗的字符数与写出的字节数；失败返回 0。 */
static size_t decode_entity(const char *input, char *out, size_t *written) {
    size_t consumed = 0;
    *written = 0;
    if (input[0] != '&') {
        return 0;
    }

    if (input[1] == '#') {
        size_t i = 2;
        int hex = 0;
        if (input[i] == 'x' || input[i] == 'X') {
            hex = 1;
            i++;
        }
        unsigned long cp = 0;
        size_t digits = 0;
        while (digits < 8) {
            char c = input[i];
            int v;
            if (c >= '0' && c <= '9') {
                v = c - '0';
            } else if (hex && c >= 'a' && c <= 'f') {
                v = c - 'a' + 10;
            } else if (hex && c >= 'A' && c <= 'F') {
                v = c - 'A' + 10;
            } else {
                break;
            }
            cp = cp * (hex ? 16UL : 10UL) + (unsigned long)v;
            i++;
            digits++;
        }
        if (digits == 0 || input[i] != ';') {
            return 0;
        }
        /* NUL 会把输出字符串截断；代理区码点不是合法的 UTF-8，两者都保持原样。 */
        if (cp == 0 || (cp >= 0xD800 && cp <= 0xDFFF)) {
            return 0;
        }
        size_t n = utf8_encode(cp, out);
        if (n == 0) {
            return 0;
        }
        *written = n;
        return i + 1;
    }

    const char *semi = strchr(input, ';');
    if (semi == NULL) {
        return 0;
    }
    consumed = (size_t)(semi - input) + 1;
    if (consumed < 3 || consumed > 12) {
        return 0;
    }

    size_t name_len = consumed - 2;
    for (size_t i = 0; i < sizeof(ENTITIES) / sizeof(ENTITIES[0]); i++) {
        if (strlen(ENTITIES[i].name) == name_len &&
            strncmp(input + 1, ENTITIES[i].name, name_len) == 0) {
            size_t n = strlen(ENTITIES[i].utf8);
            memcpy(out, ENTITIES[i].utf8, n);
            *written = n;
            return consumed;
        }
    }
    return 0;
}

static int is_block_tag(const char *tag, size_t len) {
    if (len > 0 && tag[0] == '/') {
        tag++;
        len--;
    }
    for (size_t i = 0; i < sizeof(BLOCK_TAGS) / sizeof(BLOCK_TAGS[0]); i++) {
        size_t n = strlen(BLOCK_TAGS[i]);
        if (n == len && strncmp(tag, BLOCK_TAGS[i], n) == 0) {
            return 1;
        }
    }
    return 0;
}

char *text_strip_html(const char *input) {
    if (input == NULL) {
        return strdup("");
    }

    size_t n = strlen(input);
    char *out = malloc(n + 1);
    if (out == NULL) {
        return NULL;
    }

    size_t j = 0;
    size_t i = 0;
    while (i < n) {
        char c = input[i];

        if (c == '<') {
            const char *close = strchr(input + i, '>');
            if (close == NULL) {
                out[j++] = c;
                i++;
                continue;
            }
            size_t tag_start = i + 1;
            size_t tag_len = (size_t)(close - input) - tag_start;
            if (is_block_tag(input + tag_start, tag_len) && j > 0 && out[j - 1] != '\n') {
                out[j++] = '\n';
            }
            i = (size_t)(close - input) + 1;
            continue;
        }

        if (c == '&') {
            char decoded[8];
            size_t written = 0;
            size_t consumed = decode_entity(input + i, decoded, &written);
            if (consumed > 0 && consumed <= n - i) {
                memcpy(out + j, decoded, written);
                j += written;
                i += consumed;
                continue;
            }
            out[j++] = c;
            i++;
            continue;
        }

        out[j++] = c;
        i++;
    }
    out[j] = '\0';

    /* 折叠连续空行、去掉每行行尾空格和整体首尾空白。 */
    size_t w = 0;
    size_t newlines = 0;
    for (size_t r = 0; r < j; r++) {
        char ch = out[r];
        if (ch == '\n') {
            if (newlines >= 2) {
                continue;
            }
            while (w > 0 && (out[w - 1] == ' ' || out[w - 1] == '\t')) {
                w--;
            }
            newlines++;
            out[w++] = ch;
            continue;
        }
        newlines = 0;
        out[w++] = ch;
    }
    while (w > 0 && (out[w - 1] == '\n' || out[w - 1] == ' ' || out[w - 1] == '\t')) {
        w--;
    }
    size_t lead = 0;
    while (lead < w && (out[lead] == '\n' || out[lead] == ' ' || out[lead] == '\t')) {
        lead++;
    }
    if (lead > 0) {
        memmove(out, out + lead, w - lead);
        w -= lead;
    }
    out[w] = '\0';
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
    /* 回退到 UTF-8 字符边界，否则中文/emoji 会被切成半个字符，终端显示乱码。 */
    size_t cut = max_len;
    while (cut > 0 && ((unsigned char)input[cut] & 0xC0) == 0x80) {
        cut--;
    }
    char *out = malloc(cut + 4);
    if (out == NULL) {
        return NULL;
    }
    memcpy(out, input, cut);
    memcpy(out + cut, "...", 4);
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
