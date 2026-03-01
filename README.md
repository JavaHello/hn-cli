# hn-cli (C)

使用 C 语言实现的 Hacker News CLI：
- 获取帖子列表并显示
- 列表中为每条帖子生成一句中文总结（DeepSeek）
- 打开具体帖子后，自动调用 DeepSeek 进行中文总结和翻译
- 支持交互模式和子命令模式

## 依赖

- `cc` (支持 C11)
- `libcurl`
- `json-c`

## 构建

```bash
make
```

生成可执行文件：`./hn-cli`

## 使用

```bash
# 交互模式（默认）
./hn-cli

# 列表模式
./hn-cli list
./hn-cli list -n 20
./hn-cli list -t top -n 20
./hn-cli list --type past -n 20
./hn-cli list --type ask -n 20
./hn-cli list --type show -n 20

# 打开帖子（id 或 index）
./hn-cli open 12345678
./hn-cli open 1
```

## DeepSeek 配置

```bash
export DEEPSEEK_API_KEY="your_api_key"
# 可选，默认 https://api.deepseek.com
export DEEPSEEK_BASE_URL="https://api.deepseek.com"
# 可选，列表总结缓存文件，默认 ./.hn_cli_cache.json
export HN_CLI_CACHE_FILE="/path/to/cache.json"
```

打开帖子时会自动请求 DeepSeek `chat/completions`，输出中文总结与翻译。
列表总结按帖子 ID 缓存，缓存有效期 24 小时，过期后自动刷新。

## 测试

```bash
bash tests/test_cli.sh
```

测试使用 `tests/fixtures` 本地 mock 数据，不依赖外网。
