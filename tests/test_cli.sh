#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT_DIR"

make >/dev/null

out_help="$(./hn-cli --help)"
grep -q "Usage: hn-cli" <<<"$out_help" || { echo "FAIL: --help missing usage"; exit 1; }

tmp_cache="$(mktemp)"
rm -f "$tmp_cache"
out_list="$(HN_CLI_MOCK_DIR="$ROOT_DIR/tests/fixtures" DEEPSEEK_MOCK_FILE="$ROOT_DIR/tests/fixtures/deepseek_list_response.json" HN_CLI_CACHE_FILE="$tmp_cache" ./hn-cli list -n 1)"
grep -q "\[1\] \[42\] Show HN: Tiny CLI (id:1001)" <<<"$out_list" || { echo "FAIL: list output mismatch"; echo "$out_list"; rm -f "$tmp_cache"; exit 1; }
grep -q "总结: 这是一个简洁的命令行工具分享。" <<<"$out_list" || { echo "FAIL: list summary missing"; echo "$out_list"; rm -f "$tmp_cache"; exit 1; }
grep -q '"1001"' "$tmp_cache" || { echo "FAIL: cache file missing id entry"; cat "$tmp_cache"; rm -f "$tmp_cache"; exit 1; }

out_list_cached="$(env -u DEEPSEEK_API_KEY HN_CLI_MOCK_DIR="$ROOT_DIR/tests/fixtures" HN_CLI_CACHE_FILE="$tmp_cache" ./hn-cli list -n 1)"
grep -q "总结: 这是一个简洁的命令行工具分享。" <<<"$out_list_cached" || { echo "FAIL: cache summary not used"; echo "$out_list_cached"; rm -f "$tmp_cache"; exit 1; }
rm -f "$tmp_cache"

out_open="$(HN_CLI_MOCK_DIR="$ROOT_DIR/tests/fixtures" ./hn-cli open 1001)"
grep -q "中文总结与翻译:" <<<"$out_open" || { echo "FAIL: open missing chinese header"; echo "$out_open"; exit 1; }
grep -q "中文摘要: 这是一个 Tiny CLI 项目。" <<<"$out_open" || { echo "FAIL: open missing chinese summary"; echo "$out_open"; exit 1; }

out_open_index="$(HN_CLI_MOCK_DIR="$ROOT_DIR/tests/fixtures" ./hn-cli open 1)"
grep -q "中文总结与翻译:" <<<"$out_open_index" || { echo "FAIL: open index missing chinese header"; echo "$out_open_index"; exit 1; }

out_interactive="$(printf '1\n' | HN_CLI_MOCK_DIR="$ROOT_DIR/tests/fixtures" ./hn-cli)"
grep -q "输入序号打开帖子" <<<"$out_interactive" || { echo "FAIL: interactive missing prompt"; echo "$out_interactive"; exit 1; }
grep -q "中文总结与翻译:" <<<"$out_interactive" || { echo "FAIL: interactive missing chinese output"; echo "$out_interactive"; exit 1; }

tmp_mock="$(mktemp -d)"
cp "$ROOT_DIR/tests/fixtures/topstories.json" "$tmp_mock/topstories.json"
cp "$ROOT_DIR/tests/fixtures/item_1001.json" "$tmp_mock/item_1001.json"
cp "$ROOT_DIR/tests/fixtures/item_2001.json" "$tmp_mock/item_2001.json"
out_no_key="$(env -u DEEPSEEK_API_KEY HN_CLI_MOCK_DIR="$tmp_mock" ./hn-cli open 1001 2>&1)"
grep -q "DEEPSEEK_API_KEY is not set" <<<"$out_no_key" || { echo "FAIL: missing key message not shown"; echo "$out_no_key"; rm -rf "$tmp_mock"; exit 1; }
grep -q "原文片段:" <<<"$out_no_key" || { echo "FAIL: fallback raw text missing"; echo "$out_no_key"; rm -rf "$tmp_mock"; exit 1; }
rm -rf "$tmp_mock"

echo "PASS"
