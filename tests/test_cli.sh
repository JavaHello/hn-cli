#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT_DIR"

make >/dev/null

FIXTURES="$ROOT_DIR/tests/fixtures"
LIST_MOCK="$FIXTURES/deepseek_list_response.json"
STREAM_MOCK="$FIXTURES/deepseek_stream_response.txt"

TMP_DIR="$(mktemp -d)"
trap 'rm -rf "$TMP_DIR"' EXIT

fail() {
    echo "FAIL: $1"
    shift
    for line in "$@"; do echo "$line"; done
    exit 1
}

# 每个用例用独立的缓存文件，避免用例之间互相命中缓存。
new_cache() { mktemp -u "$TMP_DIR/cache.XXXXXX"; }

# --- 基础命令 -------------------------------------------------------------

out_help="$(./hn-cli --help)"
grep -q "Usage: hn-cli" <<<"$out_help" || fail "--help missing usage" "$out_help"
grep -q -- "-p, --page" <<<"$out_help" || fail "--help missing page option" "$out_help"

# --- 列表 + 一句话总结缓存 -------------------------------------------------

cache_list="$(new_cache)"
out_list="$(HN_CLI_MOCK_DIR="$FIXTURES" DEEPSEEK_MOCK_FILE="$LIST_MOCK" HN_CLI_CACHE_FILE="$cache_list" ./hn-cli list -n 1)"
grep -q "\[1\] \[42\] Show HN: Tiny CLI (id:1001)" <<<"$out_list" || fail "list output mismatch" "$out_list"
grep -q "总结: 这是一个简洁的命令行工具分享。" <<<"$out_list" || fail "list summary missing" "$out_list"
grep -q '"1001"' "$cache_list" || fail "cache file missing id entry" "$(cat "$cache_list")"

# 命中缓存后即使没有 API key 也能显示总结
out_list_cached="$(env -u DEEPSEEK_API_KEY HN_CLI_MOCK_DIR="$FIXTURES" HN_CLI_CACHE_FILE="$cache_list" ./hn-cli list -n 1)"
grep -q "总结: 这是一个简洁的命令行工具分享。" <<<"$out_list_cached" || fail "cache summary not used" "$out_list_cached"

# --- 其它列表类型 ---------------------------------------------------------

tmp_past_mock="$(mktemp -d "$TMP_DIR/past.XXXXXX")"
cp "$FIXTURES/topstories.json" "$tmp_past_mock/topstories.json"
cp "$FIXTURES/item_3001.json" "$tmp_past_mock/item_3001.json"
cp "$FIXTURES/algolia_paststories.json" "$tmp_past_mock/algolia_paststories.json"
out_list_past="$(HN_CLI_MOCK_DIR="$tmp_past_mock" DEEPSEEK_MOCK_FILE="$LIST_MOCK" HN_CLI_CACHE_FILE="$(new_cache)" ./hn-cli list -t past -n 1)"
grep -q "\[1\] \[88\] Ask HN: What did you build years ago? (id:3001)" <<<"$out_list_past" || fail "list -t past output mismatch" "$out_list_past"

out_list_ask="$(HN_CLI_MOCK_DIR="$FIXTURES" DEEPSEEK_MOCK_FILE="$LIST_MOCK" HN_CLI_CACHE_FILE="$(new_cache)" ./hn-cli list --type ask -n 1)"
grep -q "\[1\] \[123\] Ask HN: Best terminal workflow? (id:4001)" <<<"$out_list_ask" || fail "list --type ask output mismatch" "$out_list_ask"

out_list_show="$(HN_CLI_MOCK_DIR="$FIXTURES" DEEPSEEK_MOCK_FILE="$LIST_MOCK" HN_CLI_CACHE_FILE="$(new_cache)" ./hn-cli list --type show -n 1)"
grep -q "\[1\] \[256\] Show HN: Retro Desktop App (id:5001)" <<<"$out_list_show" || fail "list --type show output mismatch" "$out_list_show"

out_bad_type="$(HN_CLI_MOCK_DIR="$FIXTURES" ./hn-cli list -t unknown -n 1 2>&1 || true)"
grep -q "error: invalid type 'unknown'" <<<"$out_bad_type" || fail "invalid type error missing" "$out_bad_type"

out_bad_page="$(HN_CLI_MOCK_DIR="$FIXTURES" ./hn-cli list -p 0 2>&1 || true)"
grep -q "error: invalid page number" <<<"$out_bad_page" || fail "invalid page error missing" "$out_bad_page"

# --- 翻页 -----------------------------------------------------------------

page_mock="$(mktemp -d "$TMP_DIR/page.XXXXXX")"
echo '[1001,1002,1001,1002]' >"$page_mock/topstories.json"
cp "$FIXTURES/item_1001.json" "$FIXTURES/item_1002.json" "$page_mock/"

out_page2="$(HN_CLI_MOCK_DIR="$page_mock" DEEPSEEK_MOCK_FILE="$LIST_MOCK" HN_CLI_CACHE_FILE="$(new_cache)" ./hn-cli list -n 2 -p 2)"
grep -q "第 2 页" <<<"$out_page2" || fail "page header missing" "$out_page2"
# 编号必须跨页连续：第二页从 [3] 开始
grep -q "\[3\] \[42\] Show HN: Tiny CLI (id:1001)" <<<"$out_page2" || fail "page 2 numbering wrong" "$out_page2"
grep -q "\[4\] \[17\] Ask HN: C parsing tips (id:1002)" <<<"$out_page2" || fail "page 2 second item wrong" "$out_page2"
grep -q "上一页: -p 1" <<<"$out_page2" || fail "page 2 missing prev hint" "$out_page2"

out_page1="$(HN_CLI_MOCK_DIR="$page_mock" DEEPSEEK_MOCK_FILE="$LIST_MOCK" HN_CLI_CACHE_FILE="$(new_cache)" ./hn-cli list -n 2 -p 1)"
grep -q "\[1\] \[42\] Show HN: Tiny CLI (id:1001)" <<<"$out_page1" || fail "page 1 numbering wrong" "$out_page1"
grep -q "\[2\] \[17\] Ask HN: C parsing tips (id:1002)" <<<"$out_page1" || fail "page 1 second item wrong" "$out_page1"
grep -q "下一页: -p 2" <<<"$out_page1" || fail "page 1 missing next hint" "$out_page1"

out_empty_page="$(HN_CLI_MOCK_DIR="$page_mock" DEEPSEEK_MOCK_FILE="$LIST_MOCK" HN_CLI_CACHE_FILE="$(new_cache)" ./hn-cli list -n 2 -p 9)"
grep -q "没有更多了" <<<"$out_empty_page" || fail "empty page message missing" "$out_empty_page"

# --- 打开帖子：流式 + 完整总结缓存 ----------------------------------------

out_open="$(HN_CLI_MOCK_DIR="$FIXTURES" HN_CLI_CACHE_FILE="$(new_cache)" ./hn-cli open 1001)"
grep -q "中文总结与翻译:" <<<"$out_open" || fail "open missing chinese header" "$out_open"
grep -q "中文摘要: 这是一个 Tiny CLI 项目。" <<<"$out_open" || fail "open missing chinese summary" "$out_open"

out_open_stream="$(DEEPSEEK_MOCK_FILE="$STREAM_MOCK" HN_CLI_MOCK_DIR="$FIXTURES" HN_CLI_CACHE_FILE="$(new_cache)" ./hn-cli open 1001)"
grep -q "中文翻译要点: 作者分享了一个简洁的命令行工具。" <<<"$out_open_stream" || fail "open stream mock missing chinese translation" "$out_open_stream"

out_open_index="$(HN_CLI_MOCK_DIR="$FIXTURES" HN_CLI_CACHE_FILE="$(new_cache)" ./hn-cli open 1)"
grep -q "中文总结与翻译:" <<<"$out_open_index" || fail "open index missing chinese header" "$out_open_index"

# 第一次打开写入缓存，第二次没有 API key 也应直接读到缓存内容
cache_open="$(new_cache)"
out_open_first="$(HN_CLI_MOCK_DIR="$FIXTURES" HN_CLI_CACHE_FILE="$cache_open" ./hn-cli open 1001)"
grep -q "中文摘要: 这是一个 Tiny CLI 项目。" <<<"$out_open_first" || fail "open first run summary missing" "$out_open_first"
grep -q '"detail_zh"' "$cache_open" || fail "detail summary not cached" "$(cat "$cache_open")"

out_open_second="$(env -u DEEPSEEK_API_KEY HN_CLI_MOCK_DIR="$FIXTURES" HN_CLI_CACHE_FILE="$cache_open" ./hn-cli open 1001)"
grep -q "本地缓存" <<<"$out_open_second" || fail "second open did not use cache" "$out_open_second"
grep -q "中文翻译要点: 作者分享了一个简洁的命令行工具。" <<<"$out_open_second" || fail "cached detail content missing" "$out_open_second"

# --refresh 必须绕过缓存
out_open_refresh="$(HN_CLI_MOCK_DIR="$FIXTURES" HN_CLI_CACHE_FILE="$cache_open" ./hn-cli open --refresh 1001)"
if grep -q "本地缓存" <<<"$out_open_refresh"; then
    fail "--refresh still served from cache" "$out_open_refresh"
fi
grep -q "中文摘要: 这是一个 Tiny CLI 项目。" <<<"$out_open_refresh" || fail "--refresh did not regenerate" "$out_open_refresh"

# open --id 让 <=500 的数字按真实 item id 解释
id_mock="$(mktemp -d "$TMP_DIR/itemid.XXXXXX")"
cp "$FIXTURES/topstories.json" "$id_mock/topstories.json"
printf '{"id":1,"title":"Y Combinator","by":"pg","score":1,"text":"<p>hello</p>"}\n' >"$id_mock/item_1.json"
out_as_index="$(HN_CLI_MOCK_DIR="$id_mock" HN_CLI_CACHE_FILE="$(new_cache)" ./hn-cli open 1 2>&1 || true)"
grep -q "mock file not found" <<<"$out_as_index" || fail "open 1 should be treated as index" "$out_as_index"
out_forced_id="$(HN_CLI_MOCK_DIR="$id_mock" HN_CLI_CACHE_FILE="$(new_cache)" ./hn-cli open --id 1 2>&1)"
grep -q "中文总结与翻译:" <<<"$out_forced_id" || fail "open --id 1 did not open real item" "$out_forced_id"

# --- past 分页（Algolia） --------------------------------------------------

past_page_mock="$(mktemp -d "$TMP_DIR/pastpage.XXXXXX")"
cat >"$past_page_mock/algolia_paststories.json" <<'EOF'
{"hits":[{"objectID":"3001"},{"objectID":"1001"},{"objectID":"1002"},{"objectID":"1001"}]}
EOF
cp "$FIXTURES/item_1001.json" "$FIXTURES/item_1002.json" "$FIXTURES/item_3001.json" "$past_page_mock/"

out_past_page2="$(HN_CLI_MOCK_DIR="$past_page_mock" DEEPSEEK_MOCK_FILE="$LIST_MOCK" HN_CLI_CACHE_FILE="$(new_cache)" ./hn-cli list -t past -n 2 -p 2)"
grep -q "\[3\] .* (id:1002)" <<<"$out_past_page2" || fail "past page 2 first item wrong" "$out_past_page2"
grep -q "\[4\] .* (id:1001)" <<<"$out_past_page2" || fail "past page 2 second item wrong" "$out_past_page2"

# --- 缓存过期语义 ---------------------------------------------------------

now="$(date +%s)"

# 超过 TTL 的列表总结必须重新生成
expired_cache="$(new_cache)"
printf '{"1001":{"summary_zh":"过期的总结","summary_at":%s}}\n' "$((now - 90000))" >"$expired_cache"
out_expired="$(HN_CLI_MOCK_DIR="$FIXTURES" DEEPSEEK_MOCK_FILE="$LIST_MOCK" HN_CLI_CACHE_FILE="$expired_cache" ./hn-cli list -n 1)"
grep -q "总结: 这是一个简洁的命令行工具分享。" <<<"$out_expired" || fail "expired summary was reused" "$out_expired"

# 完全没有时间戳的条目按过期处理，不能永久命中
no_stamp_cache="$(new_cache)"
printf '{"1001":{"summary_zh":"没有时间戳的总结"}}\n' >"$no_stamp_cache"
out_no_stamp="$(HN_CLI_MOCK_DIR="$FIXTURES" DEEPSEEK_MOCK_FILE="$LIST_MOCK" HN_CLI_CACHE_FILE="$no_stamp_cache" ./hn-cli list -n 1)"
grep -q "总结: 这是一个简洁的命令行工具分享。" <<<"$out_no_stamp" || fail "stamp-less entry was reused" "$out_no_stamp"

# 完整总结的 TTL 比列表总结长：25 小时前的 detail 命中，25 小时前的 summary 过期
split_cache="$(new_cache)"
printf '{"1001":{"summary_zh":"过期的列表总结","summary_at":%s,"detail_zh":"一天前的完整总结","detail_at":%s}}\n' \
    "$((now - 90000))" "$((now - 90000))" >"$split_cache"
out_split_detail="$(env -u DEEPSEEK_API_KEY HN_CLI_MOCK_DIR="$FIXTURES" HN_CLI_CACHE_FILE="$split_cache" ./hn-cli open 1001)"
grep -q "一天前的完整总结" <<<"$out_split_detail" || fail "detail cache should outlive summary ttl" "$out_split_detail"
out_split_summary="$(HN_CLI_MOCK_DIR="$FIXTURES" DEEPSEEK_MOCK_FILE="$LIST_MOCK" HN_CLI_CACHE_FILE="$split_cache" ./hn-cli list -n 1)"
grep -q "总结: 这是一个简洁的命令行工具分享。" <<<"$out_split_summary" || fail "summary should expire before detail" "$out_split_summary"

# --- 交互模式 -------------------------------------------------------------

out_interactive="$(printf '1\n' | HN_CLI_MOCK_DIR="$FIXTURES" HN_CLI_CACHE_FILE="$(new_cache)" ./hn-cli)"
grep -q "\[序号\]打开帖子" <<<"$out_interactive" || fail "interactive missing prompt" "$out_interactive"
grep -q "中文总结与翻译:" <<<"$out_interactive" || fail "interactive missing chinese output" "$out_interactive"

# 交互翻页：第 1 页 30 条，第 2 页 1 条，用全局序号 31 打开
paging_mock="$(mktemp -d "$TMP_DIR/interactive.XXXXXX")"
{
    printf '['
    for _ in $(seq 1 30); do printf '1002,'; done
    printf '1001]'
} >"$paging_mock/topstories.json"
cp "$FIXTURES/item_1001.json" "$FIXTURES/item_1002.json" "$paging_mock/"

out_interactive_page="$(printf 'n\n31\nq\n' | HN_CLI_MOCK_DIR="$paging_mock" DEEPSEEK_MOCK_FILE="$LIST_MOCK" HN_CLI_CACHE_FILE="$(new_cache)" ./hn-cli 2>&1)"
grep -q "第 2 页" <<<"$out_interactive_page" || fail "interactive next page missing" "$out_interactive_page"
grep -q "\[31\] \[42\] Show HN: Tiny CLI (id:1001)" <<<"$out_interactive_page" || fail "interactive page 2 item missing" "$out_interactive_page"
grep -q "中文总结与翻译:" <<<"$out_interactive_page" || fail "interactive open by global index failed" "$out_interactive_page"

out_interactive_prev="$(printf 'p\nq\n' | HN_CLI_MOCK_DIR="$paging_mock" DEEPSEEK_MOCK_FILE="$LIST_MOCK" HN_CLI_CACHE_FILE="$(new_cache)" ./hn-cli 2>&1)"
grep -q "已经是第一页" <<<"$out_interactive_prev" || fail "interactive prev from page 1 not guarded" "$out_interactive_prev"

out_interactive_empty="$(printf 'n\nn\nq\n' | HN_CLI_MOCK_DIR="$FIXTURES" DEEPSEEK_MOCK_FILE="$LIST_MOCK" HN_CLI_CACHE_FILE="$(new_cache)" ./hn-cli 2>&1)"
grep -q "没有更多了" <<<"$out_interactive_empty" || fail "interactive empty page message missing" "$out_interactive_empty"

# --- 缓存路径：XDG 默认位置 + 旧缓存迁移 ----------------------------------

legacy_dir="$(mktemp -d "$TMP_DIR/legacy.XXXXXX")"
printf '{"1001":{"summary_zh":"旧缓存总结","updated_at":%s}}\n' "$(date +%s)" >"$legacy_dir/.hn_cli_cache.json"
out_legacy="$(cd "$legacy_dir" && XDG_CACHE_HOME="$TMP_DIR/xdg" HN_CLI_MOCK_DIR="$FIXTURES" DEEPSEEK_MOCK_FILE="$LIST_MOCK" "$ROOT_DIR/hn-cli" list -n 1)"
grep -q "总结: 旧缓存总结" <<<"$out_legacy" || fail "legacy cache not read" "$out_legacy"
test -f "$TMP_DIR/xdg/hn-cli/cache.json" || fail "cache not written to XDG dir" "$(ls -R "$TMP_DIR/xdg" 2>&1 || true)"
grep -q '"detail_zh"\|"summary_zh"' "$TMP_DIR/xdg/hn-cli/cache.json" || fail "migrated cache content unexpected" "$(cat "$TMP_DIR/xdg/hn-cli/cache.json")"

# --- 缺少 API key 时的兜底 ------------------------------------------------

no_key_mock="$(mktemp -d "$TMP_DIR/nokey.XXXXXX")"
cp "$FIXTURES/topstories.json" "$no_key_mock/topstories.json"
cp "$FIXTURES/item_1001.json" "$no_key_mock/item_1001.json"
cp "$FIXTURES/item_2001.json" "$no_key_mock/item_2001.json"
out_no_key="$(env -u DEEPSEEK_API_KEY HN_CLI_MOCK_DIR="$no_key_mock" HN_CLI_CACHE_FILE="$(new_cache)" ./hn-cli open 1001 2>&1 || true)"
grep -q "DEEPSEEK_API_KEY is not set" <<<"$out_no_key" || fail "missing key message not shown" "$out_no_key"
grep -q "原文片段:" <<<"$out_no_key" || fail "fallback raw text missing" "$out_no_key"

echo "PASS"
