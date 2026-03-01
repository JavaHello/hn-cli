#ifndef CLI_H
#define CLI_H

#include <stddef.h>

int cli_run_list(size_t limit);
int cli_run_open(long id);
int cli_run_open_index(size_t index, size_t limit);
int cli_run_interactive(size_t limit);

#endif
