#ifndef DEEPSEEK_H
#define DEEPSEEK_H

int deepseek_summarize_translate_zh(const char *source_text, char **result_text, char **error_msg);
int deepseek_summarize_one_line_zh(const char *source_text, char **result_text, char **error_msg);

#endif
