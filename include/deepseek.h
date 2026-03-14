#ifndef DEEPSEEK_H
#define DEEPSEEK_H

typedef int (*deepseek_output_callback)(const char *chunk, void *user_data);

int deepseek_summarize_translate_zh(const char *source_text, char **result_text, char **error_msg);
int deepseek_summarize_translate_zh_stream(const char *source_text, deepseek_output_callback output_callback, void *user_data, char **result_text, char **error_msg);
int deepseek_summarize_one_line_zh(const char *source_text, char **result_text, char **error_msg);

#endif
