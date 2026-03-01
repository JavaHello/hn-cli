#ifndef TEXT_H
#define TEXT_H

#include <stddef.h>

char *text_strip_html(const char *input);
char *text_truncate_copy(const char *input, size_t max_len);
char *text_join_two(const char *a, const char *b, const char *sep);

#endif
