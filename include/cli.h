#ifndef CLI_H
#define CLI_H

#include <stddef.h>

/* page 从 1 开始，编号在所有页之间连续。 */
int cli_run_list(const char *type, size_t page, size_t page_size);

/* id 是真实的 Hacker News item id。refresh 非 0 时忽略已缓存的总结强制重新生成。 */
int cli_run_open(long id);
int cli_run_open_ex(long id, int refresh);

/* index 是列表里的全局序号（从 1 开始），按 type 对应的列表查找。 */
int cli_run_open_index(const char *type, size_t index);

int cli_run_interactive(const char *type, size_t page_size);

#endif
