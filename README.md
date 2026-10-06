# hn-cli (C)

使用 C 语言实现的 Hacker News CLI：
- 获取帖子列表并分页浏览（上一页 / 下一页）
- 列表中为每条帖子生成一句中文总结（DeepSeek）
- 打开具体帖子后，自动调用 DeepSeek 进行中文总结和翻译
- 总结结果缓存到本地，重复查看不再重复调用 API
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
# 交互模式（默认），支持翻页
./hn-cli

# 列表模式
./hn-cli list
./hn-cli list -n 20
./hn-cli list -t top -n 20
./hn-cli list --type past -n 20
./hn-cli list --type ask -n 20
./hn-cli list --type show -n 20

# 翻页：第 2 页，每页 20 条（编号从 [21] 开始）
./hn-cli list -n 20 -p 2
./hn-cli list --page 2

# 打开帖子（id 或列表序号）
./hn-cli open 12345678
./hn-cli open 1          # 1 <= 500，按列表序号解释
./hn-cli open --id 1     # 强制按真实 item id 解释
./hn-cli open --refresh 12345678   # 忽略缓存重新生成
```

### 交互模式

```
[序号]打开帖子  [n]下一页  [p]上一页  [r]重看上一个（忽略缓存）  [q]退出
```

序号在所有页之间连续：第 2 页（每页 30 条）显示的是 `[31]` 起，输入 `31` 即可打开。
`past` 类型走 Algolia 服务端分页，最多翻到前 1000 条。

## DeepSeek 配置

```bash
export DEEPSEEK_API_KEY="your_api_key"
# 可选，默认 https://api.deepseek.com
export DEEPSEEK_BASE_URL="https://api.deepseek.com"
# 可选，默认 deepseek-flash
export DEEPSEEK_MODEL="deepseek-flash"
```

打开帖子时会自动请求 DeepSeek 流式 `chat/completions`，并在终端中边接收边输出中文总结与翻译。

## 缓存

列表的一句话总结和 `open` 的完整总结翻译都会缓存到本地：

| 环境变量 | 说明 |
| --- | --- |
| `HN_CLI_CACHE_FILE` | 缓存文件路径，默认 `$XDG_CACHE_HOME/hn-cli/cache.json`（通常是 `~/.cache/hn-cli/cache.json`） |
| `HN_CLI_CACHE_TTL` | 覆盖缓存有效期（秒），`0` 表示永不过期 |

默认有效期：一句话总结 24 小时，完整总结翻译 7 天（后者是一次昂贵调用，留得久一些）。
旧的 `./.hn_cli_cache.json` 会在首次运行时自动复制到新位置（原文件保留）。
缓存写入采用「临时文件 + rename」的原子写，中途 Ctrl-C 不会损坏缓存文件。

## 其它环境变量

| 环境变量 | 说明 |
| --- | --- |
| `HN_CLI_MOCK_DIR` | 用本地文件替代网络请求（测试用） |
| `DEEPSEEK_MOCK_FILE` | 用本地文件替代 DeepSeek 响应（测试用） |

## 测试

```bash
bash tests/test_cli.sh
```

测试使用 `tests/fixtures` 本地 mock 数据，不依赖外网，覆盖列表、翻页、缓存命中/过期、
交互模式、`past` 分页和缺少 API key 时的兜底行为。
