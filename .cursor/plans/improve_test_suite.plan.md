---
name: ""
overview: ""
todos: []
isProject: false
---

# Improve pg_enhanced_query_logging Test Suite

## Overview

Improve the pg_enhanced_query_logging test suite by fixing existing test bugs, adding Meson build support, expanding automated TAP test coverage for untested features, cleaning up test documentation, and adding CI/CD infrastructure -- all using standard PostgreSQL testing infrastructure compatible with macOS and Linux.

## Todos

- **fix-test-bugs**: Fix 5 known test bugs: version mismatch in expected/01_basic.out (#32), file-read-after-reset in t/001_basic_logging.pl (#39), weak rate-limit assertion in t/002_rate_limiting.pl (#40), no-op poll_query_until in t/001_basic_logging.pl (#41), stale known issue in test/TESTING.md (#34)
- **add-meson-build**: Create meson.build with shared module, regress tests, TAP tests, and extension data declarations
- **create-test-helper**: Create t/PeqlNode.pm shared helper for node setup, extension creation, and reliable log reading after reset
- **add-tap-nested**: Create t/004_nested_logging.pl: test peql.log_nested on/off with PL/pgSQL functions
- **add-tap-params**: Create t/005_parameter_values.pl: test parameter logging on/off, NULL values, prepared statements
- **add-tap-plan**: Create t/006_query_plan.pl: test EXPLAIN output in text/JSON format, plan on/off
- **add-tap-planning-time**: Create t/007_planning_time.pl: test track_planning on/off with full verbosity
- **add-tap-min-duration**: Create t/008_min_duration.pl: test duration threshold filtering with pg_sleep
- **add-tap-session-rate**: Create t/009_session_rate_limit.pl: test session-mode rate limiting all-or-nothing behavior
- **add-tap-edge-cases**: Create t/010_edge_cases.pl: test Filesort_on_disk, Temp_table_on_disk, Mem_allocated, Schema field changes
- **update-testing-docs**: Update test/TESTING.md: remove stale known issue, document new TAP tests and Meson build
- **add-ci-cd**: Create .github/workflows/test.yml for automated CI on Ubuntu and macOS with PG 17/18

---

## Current State

The extension has two test layers, both using standard PostgreSQL infrastructure:

- **pg_regress SQL tests** (3 files in `sql/` + `expected/`): Test GUC defaults, validation, and extension load/unload. These are solid but have a known version mismatch bug.
- **TAP tests** (3 files in `t/`): Test actual log output behavior using `PostgreSQL::Test::Cluster`. These cover core logging, rate limiting, and extended metrics, but leave several features untested.
- **Manual tests** (`test/TESTING.md`, 649 lines): Comprehensive manual testing guide covering everything. Many of these scenarios should be automated.

There is no `meson.build` (upstream PG has been migrating to Meson since PG 16), and no CI/CD configuration.

---

## 1. Fix Known Test Bugs

Five open bugs directly affect tests (from `bugs_and_improvements/`):

- **#32**: `expected/01_basic.out` hardcodes `extversion = 1.0` but the control file defaults to `1.1`. The regression test fails on a fresh install. Fix: update expected output to `1.1`.
- **#39**: `t/001_basic_logging.pl` lines 57-64 -- after `pg_enhanced_query_logging_reset()` renames the log file, the test reads the original path (`$log_file`) which may not exist yet (no new query has been issued to re-create it). Fix: issue a dummy query after reset before reading, or read the rotated file.
- **#40**: `t/002_rate_limiting.pl` line 52 -- the assertion `< 200` is too weak for a `rate_limit=1000` test over 200 queries. Statistically, expect ~0-1 entries. Fix: tighten the bound (e.g., `< 10`).
- **#41**: `t/001_basic_logging.pl` lines 28-29 -- `poll_query_until('postgres', "SELECT 1", '1')` always succeeds immediately, providing no actual synchronization. Fix: replace with a brief `sleep` or a real file-existence check, or remove if unnecessary.
- **#34**: `test/TESTING.md` lines 303-306 -- documents a "known issue" for bug #21 that was already fixed. Fix: remove the stale note.

---

## 2. Add Meson Build Support

Upstream PostgreSQL (PG 16+) supports Meson alongside Make. Adding a `meson.build` file makes the extension compatible with both build systems and future-proof. The file should declare:

- The shared module build (`pg_enhanced_query_logging.c`)
- The `REGRESS` tests (same 3 SQL tests)
- The TAP tests (same 3 Perl files)
- Extension data files

This is a single `meson.build` file following the pattern used by `contrib/` extensions like `pg_stat_statements`.

---

## 3. Expand TAP Test Coverage

The following features are documented in `test/TESTING.md` with manual procedures but lack automated TAP tests. Each should become a new TAP test file:

### t/004_nested_logging.pl -- Nested statement logging

- Create a PL/pgSQL function with inner statements
- Test `peql.log_nested = off`: only the outer call is logged
- Test `peql.log_nested = on`: inner statements also appear as separate entries

### t/005_parameter_values.pl -- Parameter value logging

- Test `peql.log_parameter_values = on` with prepared statements: `# Parameters:` line appears
- Test NULL parameter values: shown as `NULL`
- Test `peql.log_parameter_values = off`: no `# Parameters:` line

### t/006_query_plan.pl -- EXPLAIN plan output

- Test `peql.log_query_plan = on` + text format: `# Plan:` line with EXPLAIN output
- Test `peql.log_query_plan = on` + JSON format: JSON EXPLAIN output
- Test `peql.log_query_plan = off`: no plan output

### t/007_planning_time.pl -- Planning time tracking

- Test `peql.track_planning = on` + full verbosity: `Plan_time:` line present
- Test `peql.track_planning = off`: no `Plan_time:` line

### t/008_min_duration.pl -- Duration threshold filtering

- Test `peql.log_min_duration = 500`: fast queries not logged, slow queries (`pg_sleep`) logged
- Test `peql.log_min_duration = 0`: all queries logged
- Test `peql.log_min_duration = -1`: no queries logged (already partially tested, but a dedicated test is cleaner)

### t/009_session_rate_limit.pl -- Session-mode rate limiting

- Test `peql.rate_limit_type = 'session'` with `rate_limit > 1`: within a single session, logging is all-or-nothing
- Run multiple sessions to verify statistical behavior

### t/010_edge_cases.pl -- Plan quality edge cases

- Force disk-based sort (`work_mem = '64kB'` + large ORDER BY): `Filesort_on_disk: Yes`
- Force temp table on disk (CTE + low `work_mem`): `Temp_table_on_disk: Yes`
- Memory tracking (`peql.track_memory = on`): `Mem_allocated` present
- Schema field changes with `search_path` changes

---

## 4. Add Shared Test Helper

To reduce boilerplate across TAP tests, create a helper module:

### t/PeqlNode.pm (or inline helper sub)

A small Perl module or shared subroutine that:

- Provides `setup_peql_node()`: creates a node, configures it with standard PEQL settings, starts it, creates the extension
- Provides `reset_and_get_log($node)`: calls `pg_enhanced_query_logging_reset()`, runs a query, and returns log contents reliably (solving the file-read-after-reset timing issue from bug #39)
- Returns the log file path

This keeps each test file focused on its specific assertions. The module should use only standard Perl and `PostgreSQL::Test::`* modules, so it works on both macOS and Linux.

---

## 5. Update Test Documentation

- **Remove stale known issue** from `test/TESTING.md` lines 303-306 (bug #34)
- **Update section 20** (Automated Tests) to reference the new TAP tests
- **Add a note** about Meson build support (`meson setup build && cd build && meson test`)
- Keep the manual testing guide as a complement -- it remains useful for exploratory testing and format validation with `pt-query-digest`

---

## 6. Add CI/CD with GitHub Actions

Create `.github/workflows/test.yml` that:

- Runs on `ubuntu-latest` (and optionally `macos-latest`)
- Installs PostgreSQL 17 and 18 from packages
- Builds the extension with both Make and Meson
- Runs `make installcheck` (pg_regress) and `make prove_installcheck` (TAP)
- Archives test results as artifacts on failure

This uses only standard PostgreSQL test infrastructure and works on both platforms.

---

## Summary of Changes

- **Bug fixes** (`expected/01_basic.out`, `t/001_basic_logging.pl`, `t/002_rate_limiting.pl`, `test/TESTING.md`): Fix 5 known test bugs
- **Meson** (`meson.build`, new): Add Meson build support
- **TAP tests** (`t/004_*.pl` through `t/010_*.pl`, 7 new files): Automate manual test scenarios
- **Helper** (`t/PeqlNode.pm`, new): Shared test setup/teardown
- **Docs** (`test/TESTING.md`): Remove stale issue, update automated test section
- **CI/CD** (`.github/workflows/test.yml`, new): Automated testing on push/PR

