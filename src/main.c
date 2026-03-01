#include "cli.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void print_usage(void) {
    printf("Usage: hn-cli [list [-t TYPE|--type TYPE] [-n N] | open <id> | --help]\n");
}

int main(int argc, char **argv) {
    if (argc == 1) {
        return cli_run_interactive(30);
    }

    if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) {
        print_usage();
        return 0;
    }

    if (strcmp(argv[1], "list") == 0) {
        size_t n = 30;
        const char *type = "top";
        for (int i = 2; i < argc; i++) {
            if (strcmp(argv[i], "-n") == 0) {
                if (i + 1 >= argc) {
                    print_usage();
                    return 1;
                }
                long v = strtol(argv[++i], NULL, 10);
                if (v > 0) {
                    n = (size_t)v;
                }
                continue;
            }
            if (strcmp(argv[i], "-t") == 0 || strcmp(argv[i], "--type") == 0) {
                if (i + 1 >= argc) {
                    print_usage();
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
        return cli_run_list(type, n);
    }

    if (strcmp(argv[1], "open") == 0) {
        if (argc < 3) {
            print_usage();
            return 1;
        }
        long n = strtol(argv[2], NULL, 10);
        if (n <= 0) {
            fprintf(stderr, "error: invalid id\n");
            return 1;
        }
        if (n <= 500) {
            return cli_run_open_index((size_t)n, 30);
        }
        return cli_run_open(n);
    }

    print_usage();
    return 1;
}
