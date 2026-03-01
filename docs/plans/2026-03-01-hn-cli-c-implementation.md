# HN CLI (C) Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Build a C CLI that lists Hacker News posts, opens a post, summarizes and translates to Chinese via DeepSeek.

**Architecture:** Use modular C files: HTTP client over libcurl, HN API fetch/parsing, text cleanup, DeepSeek API adapter in a dedicated file, and CLI/argument dispatch. Keep first version minimal and resilient with graceful fallback on API errors.

**Tech Stack:** C (C11), libcurl, cJSON, Makefile, shell-based integration tests.

---

### Task 1: Project scaffold and build system

**Files:**
- Create: `include/http.h`
- Create: `include/hn_api.h`
- Create: `include/deepseek.h`
- Create: `include/text.h`
- Create: `src/http.c`
- Create: `src/hn_api.c`
- Create: `src/deepseek.c`
- Create: `src/text.c`
- Create: `src/cli.c`
- Create: `src/main.c`
- Create: `tests/test_cli.sh`
- Create: `Makefile`
- Create: `README.md`

**Step 1: Write failing test**
- Add `tests/test_cli.sh` expecting `hn-cli --help` to print usage and return 0.

**Step 2: Run test to verify it fails**
- Run: `bash tests/test_cli.sh`
- Expected: FAIL because binary not built.

**Step 3: Write minimal implementation**
- Add Makefile and minimal `main.c` usage output.

**Step 4: Run test to verify it passes**
- Run: `bash tests/test_cli.sh`
- Expected: PASS for help behavior.

### Task 2: Implement HN list and open flow

**Files:**
- Modify: `src/http.c`, `src/hn_api.c`, `src/cli.c`, `src/main.c`
- Test: `tests/test_cli.sh`

**Step 1: Write failing test**
- Add test for `hn-cli list -n 1` under mocked mode (fixture JSON via env var).

**Step 2: Run to verify fail**
- Run: `bash tests/test_cli.sh`
- Expected: FAIL for list behavior.

**Step 3: Minimal implementation**
- Implement list fetch/parsing and render `[index] [score] title (id:xxx)`.

**Step 4: Verify pass**
- Run tests again.

### Task 3: Implement DeepSeek summary+translation (single dedicated file)

**Files:**
- Modify: `src/deepseek.c`, `src/cli.c`, `include/deepseek.h`
- Test: `tests/test_cli.sh`

**Step 1: Write failing test**
- Add mock mode test for `hn-cli open <id>` requiring Chinese output label.

**Step 2: Run fail**
- Run: `bash tests/test_cli.sh`

**Step 3: Minimal implementation**
- DeepSeek file handles request/response only.
- CLI combines post text + max 20 comments, invokes DeepSeek and prints result.

**Step 4: Verify pass**
- Run tests and then manual run instructions.

### Task 4: Hardening and docs

**Files:**
- Modify: `README.md`, `tests/test_cli.sh`, optional source files for error handling.

**Step 1: Write failing test**
- Missing API key should return explicit error for open action.

**Step 2: Run fail**
- Run test script.

**Step 3: Minimal implementation**
- Add key validation and fallback message.

**Step 4: Verify pass**
- Run `bash tests/test_cli.sh` and `make`.
