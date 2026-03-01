#include "cli.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void print_usage(void) {
    printf("Usage: hn-cli [list [-n N] | open <id> | --help]\n");
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
        if (argc == 4 && strcmp(argv[2], "-n") == 0) {
            long v = strtol(argv[3], NULL, 10);
            if (v > 0) {
                n = (size_t)v;
            }
        }
        return cli_run_list(n);
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
