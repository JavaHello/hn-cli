#include "cli.h"
#include "http.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEFAULT_PAGE_SIZE 30
/* 小于等于这个值的数字在 open 时按列表序号解释，否则按 item id 解释。 */
#define INDEX_ID_THRESHOLD 500

static void print_usage(void) {
    printf(
        "Usage: hn-cli [list [-t TYPE] [-n N] [-p PAGE] | open <id|index> | --help]\n"
        "\n"
        "  (无参数)            交互模式，支持 n/p 翻页\n"
        "  list                列出帖子，默认 top 类型、每页 30 条\n"
        "    -t, --type TYPE   top | past | ask | show\n"
        "    -n N              每页条数\n"
        "    -p, --page PAGE   页码，从 1 开始\n"
        "  open <id|index>     打开帖子；数字 <= %d 时按列表序号，否则按 item id\n"
        "    --id              强制按 item id 解释（用于打开真实的 1~%d 号帖子）\n"
        "    --refresh         忽略本地缓存，重新生成中文总结\n",
        INDEX_ID_THRESHOLD, INDEX_ID_THRESHOLD);
}

static int parse_size(const char *text, size_t *out) {
    char *end = NULL;
    long v = strtol(text, &end, 10);
    if (end == text || *end != '\0' || v <= 0) {
        return -1;
    }
    *out = (size_t)v;
    return 0;
}

static int run_list(int argc, char **argv) {
    size_t page_size = DEFAULT_PAGE_SIZE;
    size_t page = 1;
    const char *type = "top";

    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "-n") == 0) {
            if (i + 1 >= argc || parse_size(argv[++i], &page_size) != 0) {
                fprintf(stderr, "error: invalid -n value\n");
                return 1;
            }
            continue;
        }
        if (strcmp(argv[i], "-p") == 0 || strcmp(argv[i], "--page") == 0) {
            if (i + 1 >= argc || parse_size(argv[++i], &page) != 0) {
                fprintf(stderr, "error: invalid page number\n");
                return 1;
            }
            continue;
        }
        if (strcmp(argv[i], "-t") == 0 || strcmp(argv[i], "--type") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "error: missing value for %s\n", argv[i]);
                return 1;
            }
            type = argv[++i];
            continue;
        }
        print_usage();
        return 1;
    }

    if (strcmp(type, "top") != 0 && strcmp(type, "past") != 0 && strcmp(type, "ask") != 0 &&
        strcmp(type, "show") != 0) {
        fprintf(stderr, "error: invalid type '%s'\n", type);
        return 1;
    }
    return cli_run_list(type, page, page_size);
}

int main(int argc, char **argv) {
    http_global_init();

    if (argc == 1) {
        return cli_run_interactive("top", DEFAULT_PAGE_SIZE);
    }

    if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) {
        print_usage();
        return 0;
    }

    if (strcmp(argv[1], "list") == 0) {
        return run_list(argc, argv);
    }

    if (strcmp(argv[1], "open") == 0) {
        int refresh = 0;
        int force_id = 0;
        const char *value = NULL;
        for (int i = 2; i < argc; i++) {
            if (strcmp(argv[i], "--refresh") == 0) {
                refresh = 1;
                continue;
            }
            if (strcmp(argv[i], "--id") == 0) {
                force_id = 1;
                continue;
            }
            if (value == NULL) {
                value = argv[i];
                continue;
            }
            print_usage();
            return 1;
        }
        if (value == NULL) {
            print_usage();
            return 1;
        }

        char *end = NULL;
        long n = strtol(value, &end, 10);
        if (end == value || *end != '\0' || n <= 0) {
            fprintf(stderr, "error: invalid id\n");
            return 1;
        }
        if (!force_id && n <= INDEX_ID_THRESHOLD) {
            return cli_run_open_index("top", (size_t)n);
        }
        return cli_run_open_ex(n, refresh);
    }

    print_usage();
    return 1;
}
