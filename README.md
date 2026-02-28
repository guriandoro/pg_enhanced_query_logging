# pg_enhanced_query_logging

A PostgreSQL extension that produces **pt-query-digest-compatible slow query logs** with extended PostgreSQL-specific metrics. Modeled after [Percona Server's improved slow query log](https://docs.percona.com/percona-server/innovation-release/slow-extended.html), it gives PostgreSQL users the same rich query analysis workflow that MySQL/Percona Server users have enjoyed for years.

The extension hooks into the executor pipeline to capture timing, buffer I/O, WAL, JIT, and row-count metrics for every query that exceeds a configurable duration threshold, then writes them to a dedicated log file that [`pt-query-digest`](https://docs.percona.com/percona-toolkit/pt-query-digest.html) can parse directly.

## Features

- **pt-query-digest compatibility** -- output format matches the MySQL slow query log so `pt-query-digest --type slowlog` works out of the box
- **Three verbosity levels** -- from lightweight timing-only (`minimal`) to full buffer/WAL/JIT instrumentation (`full`)
- **Plan quality indicators** -- `Full_scan`, `Filesort`, `Filesort_on_disk`, `Temp_table`, `Temp_table_on_disk` derived from the actual plan tree
- **Rate limiting** -- per-session or per-query sampling with an always-log override for very slow queries
- **Buffer, WAL, and I/O timing metrics** -- shared/local/temp block counts, read/write times, WAL records and bytes
- **JIT metrics** -- function count, generation/optimization/inlining/emission times
- **Planning time tracking** -- separate planner hook measures planning wall-clock time
- **Memory context tracking** -- experimental per-query memory allocation measurement
- **Utility statement logging** -- optional logging of DDL and other utility statements
- **Nested statement logging** -- optional logging of statements inside PL/pgSQL functions
- **Parameter value logging** -- optional inclusion of bind parameter values
- **EXPLAIN plan inclusion** -- optional EXPLAIN ANALYZE output embedded in the log entry

## Requirements

- PostgreSQL 17 or later (tested on 18.x)
- C compiler and `pg_config` in your `PATH` (or specified explicitly)
- Optional: [Percona Toolkit](https://docs.percona.com/percona-toolkit/) for `pt-query-digest`

### Installing pt-query-digest

If you don't have `pt-query-digest` installed, you can grab the standalone script:

```bash
cd ~/.local/bin
curl -LO https://percona.com/get/pt-query-digest
chmod +x pt-query-digest
export PATH="$HOME/.local/bin:$PATH"
```

To make the `PATH` change permanent, add `export PATH="$HOME/.local/bin:$PATH"` to your shell profile (`~/.bashrc`, `~/.zshrc`, etc.).

## Installation

### Building from Source

```bash
git clone https://github.com/guriandoro/pg_enhanced_query_logging.git
cd pg_enhanced_query_logging

# Build (auto-detects pg_config in PATH)
make USE_PGXS=1

# Or specify pg_config explicitly
make USE_PGXS=1 PG_CONFIG=/path/to/pg_config

# Install into the PostgreSQL installation
sudo make install USE_PGXS=1
```

This installs:
- `pg_enhanced_query_logging.so` (or `.dylib`) into `$(pg_config --pkglibdir)`
- `pg_enhanced_query_logging.control` and `pg_enhanced_query_logging--1.0.sql` into `$(pg_config --sharedir)/extension/`

### Loading the Extension

Add to `postgresql.conf`:

```
shared_preload_libraries = 'pg_enhanced_query_logging'
```

Restart PostgreSQL, then create the extension in each database where you want the SQL helper functions:

```sql
CREATE EXTENSION pg_enhanced_query_logging;
```

> **Note:** The extension's logging hooks are active for all databases as soon as the module is loaded via `shared_preload_libraries`. The `CREATE EXTENSION` step only installs the SQL-callable `pg_enhanced_query_logging_reset()` function.

## Quick Start

Add these lines to `postgresql.conf` and restart:

```
shared_preload_libraries = 'pg_enhanced_query_logging'
peql.log_min_duration = 100    # log queries slower than 100ms
peql.log_verbosity = 'full'    # include all extended metrics
```

Run some queries, then analyze with pt-query-digest:

```bash
pt-query-digest --type slowlog $(pg_config --logdir)/peql-slow.log
```

## Configuration Reference

All GUC variables are prefixed with `peql.` and can be set in `postgresql.conf` or at runtime (within their context restrictions).

### Core Settings

| GUC | Type | Default | Context | Description |
|-----|------|---------|---------|-------------|
| `peql.enabled` | bool | `on` | SUSET | Master on/off switch for the extension |
| `peql.log_min_duration` | int (ms) | `-1` | SUSET | Minimum execution time to log a query. `-1` disables logging entirely; `0` logs all queries |
| `peql.log_directory` | string | `""` | SIGHUP | Directory for the log file. Empty string uses PostgreSQL's `log_directory` |
| `peql.log_filename` | string | `"peql-slow.log"` | SIGHUP | Name of the slow query log file |
| `peql.log_verbosity` | enum | `standard` | SUSET | Detail level: `minimal`, `standard`, or `full` |

### Statement Filtering

| GUC | Type | Default | Context | Description |
|-----|------|---------|---------|-------------|
| `peql.log_utility` | bool | `off` | SUSET | Log utility (DDL) statements via the ProcessUtility hook |
| `peql.log_nested` | bool | `off` | SUSET | Log statements nested inside PL/pgSQL functions and procedures |

### Rate Limiting

| GUC | Type | Default | Context | Description |
|-----|------|---------|---------|-------------|
| `peql.rate_limit` | int | `1` | SUSET | Log every Nth query or session. `1` = log all eligible queries |
| `peql.rate_limit_type` | enum | `query` | SUSET | Sampling mode: `session` (decide once per backend) or `query` (decide per statement) |
| `peql.rate_limit_always_log_duration` | int (ms) | `10000` | SUSET | Queries slower than this bypass the rate limiter entirely. `0` = bypass for all queries, `-1` = disabled |

### Extended Tracking

| GUC | Type | Default | Context | Description |
|-----|------|---------|---------|-------------|
| `peql.track_io_timing` | bool | `on` | SUSET | Include block read/write times (requires PostgreSQL's `track_io_timing = on`) |
| `peql.track_wal` | bool | `on` | SUSET | Include WAL usage metrics (records, bytes, full-page images) |
| `peql.track_memory` | bool | `off` | SUSET | Include memory context allocation (experimental, adds overhead) |
| `peql.track_planning` | bool | `off` | SUSET | Track and log planning time separately from execution time |

### Query Plan and Parameters

| GUC | Type | Default | Context | Description |
|-----|------|---------|---------|-------------|
| `peql.log_parameter_values` | bool | `off` | SUSET | Append bind parameter values to the log entry |
| `peql.log_query_plan` | bool | `off` | SUSET | Include EXPLAIN ANALYZE output in the log entry |
| `peql.log_query_plan_format` | enum | `text` | SUSET | EXPLAIN output format: `text` or `json` |

### Context Values

- **SUSET**: Can be changed by superusers at runtime with `SET` or `ALTER SYSTEM`
- **SIGHUP**: Requires a configuration reload (`pg_ctl reload` or `SELECT pg_reload_conf()`)

### Runtime Examples

```sql
-- Log all queries (including very fast ones)
SET peql.log_min_duration = 0;

-- Only log queries slower than 500ms
SET peql.log_min_duration = 500;

-- Disable logging temporarily
SET peql.log_min_duration = -1;

-- Switch to minimal output (timing and rows only)
SET peql.log_verbosity = 'minimal';

-- Enable full metrics including buffer/WAL/JIT
SET peql.log_verbosity = 'full';

-- Sample 1 in 10 queries
SET peql.rate_limit = 10;
SET peql.rate_limit_type = 'query';

-- Always log queries over 5 seconds even when sampling
SET peql.rate_limit_always_log_duration = 5000;
```

## Output Format

The log format is designed to be parsed by `pt-query-digest --type slowlog`. Every entry follows the MySQL slow query log structure with `# Key: Value` header lines.

### Minimal Verbosity

The core fields required by pt-query-digest, emitted at all verbosity levels:

```
# Time: 2026-02-27T14:30:00.123456
# User@Host: alice[alice] @ 192.168.1.10 []
# Query_time: 0.523411  Lock_time: 0.000000  Rows_sent: 42  Rows_examined: 15000
SET timestamp=1772147400;
SELECT * FROM orders WHERE status = 'pending';
```

### Standard Verbosity

Adds connection metadata:

```
# Time: 2026-02-27T14:30:00.123456
# User@Host: alice[alice] @ 192.168.1.10 []
# Thread_id: 12345  Schema: mydb.public
# Query_time: 0.523411  Lock_time: 0.000000  Rows_sent: 42  Rows_examined: 15000
# Rows_affected: 0
SET timestamp=1772147400;
SELECT * FROM orders WHERE status = 'pending';
```

### Full Verbosity

Adds buffer, WAL, JIT, plan quality, and memory metrics:

```
# Time: 2026-02-27T14:30:00.123456
# User@Host: alice[alice] @ 192.168.1.10 []
# Thread_id: 12345  Schema: mydb.public
# Query_time: 0.523411  Lock_time: 0.000000  Rows_sent: 42  Rows_examined: 15000
# Rows_affected: 0
# Shared_blks_hit: 128  Shared_blks_read: 42  Shared_blks_dirtied: 0  Shared_blks_written: 0
# Local_blks_hit: 0  Local_blks_read: 0  Local_blks_dirtied: 0  Local_blks_written: 0
# Temp_blks_read: 0  Temp_blks_written: 0
# Shared_blk_read_time: 0.001234  Shared_blk_write_time: 0.000000
# WAL_records: 0  WAL_bytes: 0  WAL_fpi: 0
# Plan_time: 0.002100
# Full_scan: Yes  Temp_table: No  Temp_table_on_disk: No  Filesort: No  Filesort_on_disk: No
# Mem_allocated: 32768
SET timestamp=1772147400;
SELECT * FROM orders WHERE status = 'pending';
```

### Field Reference

| Field | Source | Verbosity | Description |
|-------|--------|-----------|-------------|
| `Time` | `GetCurrentTimestamp()` | all | ISO-8601 timestamp with microsecond precision |
| `User@Host` | `MyProcPort` | all | Connecting user and remote host |
| `Thread_id` | `MyProcPid` | standard+ | PostgreSQL backend PID |
| `Schema` | `fetch_search_path()` | standard+ | Database and schema in `db.schema` format |
| `Query_time` | `Instrumentation.total` | all | Total execution time in seconds |
| `Lock_time` | -- | all | Reserved (always 0; PostgreSQL doesn't expose per-query lock wait time the same way MySQL does) |
| `Rows_sent` | `es_processed` (SELECT) | all | Rows returned to the client |
| `Rows_examined` | Plan tree ntuples sum | all | Rows scanned across all plan nodes |
| `Rows_affected` | `es_processed` (DML) | standard+ | Rows modified by INSERT/UPDATE/DELETE |
| `Shared_blks_hit` | `BufferUsage` | full | Shared buffer hits |
| `Shared_blks_read` | `BufferUsage` | full | Shared blocks read from disk |
| `Shared_blks_dirtied` | `BufferUsage` | full | Shared blocks dirtied |
| `Shared_blks_written` | `BufferUsage` | full | Shared blocks written |
| `Local_blks_hit` | `BufferUsage` | full | Local buffer hits |
| `Local_blks_read` | `BufferUsage` | full | Local blocks read |
| `Local_blks_dirtied` | `BufferUsage` | full | Local blocks dirtied |
| `Local_blks_written` | `BufferUsage` | full | Local blocks written |
| `Temp_blks_read` | `BufferUsage` | full | Temp blocks read |
| `Temp_blks_written` | `BufferUsage` | full | Temp blocks written |
| `Shared_blk_read_time` | `BufferUsage` | full | Time spent reading shared blocks (seconds) |
| `Shared_blk_write_time` | `BufferUsage` | full | Time spent writing shared blocks (seconds) |
| `WAL_records` | `WalUsage` | full | WAL records generated |
| `WAL_bytes` | `WalUsage` | full | WAL bytes generated |
| `WAL_fpi` | `WalUsage` | full | Full-page images written |
| `Plan_time` | Planner hook | full | Planning time in seconds (requires `peql.track_planning`) |
| `Full_scan` | Plan tree (SeqScan) | full | Whether the query performed a sequential scan |
| `Temp_table` | Plan tree (Material) | full | Whether a materialization node was used |
| `Temp_table_on_disk` | `temp_blks_written > 0` | full | Whether temp data spilled to disk |
| `Filesort` | Plan tree (Sort) | full | Whether a sort operation was performed |
| `Filesort_on_disk` | Tuplesort stats | full | Whether the sort spilled to disk |
| `Mem_allocated` | `MemoryContextMemAllocated()` | full | Bytes allocated in the query memory context (requires `peql.track_memory`) |
| `JIT_functions` | `JitInstrumentation` | full | Number of JIT-compiled functions |
| `JIT_generation_time` | `JitInstrumentation` | full | Time spent generating JIT code (seconds) |
| `JIT_inlining_time` | `JitInstrumentation` | full | Time spent inlining JIT code (seconds) |
| `JIT_optimization_time` | `JitInstrumentation` | full | Time spent optimizing JIT code (seconds) |
| `JIT_emission_time` | `JitInstrumentation` | full | Time spent emitting JIT code (seconds) |
| `Log_slow_rate_type` | GUC | all (when sampling) | Rate limit mode (`session` or `query`) |
| `Log_slow_rate_limit` | GUC | all (when sampling) | Configured rate limit value |

### Key Naming Conventions

The field names are chosen to work with pt-query-digest's `# (\w+): (\S+)` parser:

- Fields ending in `_time` are parsed as float/time metrics
- `Yes`/`No` values are parsed as booleans
- Everything else is parsed as integers

## Using with pt-query-digest

### Basic Report

```bash
pt-query-digest --type slowlog /path/to/peql-slow.log
```

### Filter by Query Time

```bash
# Only show queries slower than 1 second
pt-query-digest --type slowlog --filter '$event->{Query_time} > 1' /path/to/peql-slow.log
```

### Filter by User

```bash
pt-query-digest --type slowlog --filter '$event->{user} eq "myapp"' /path/to/peql-slow.log
```

### Review Specific Time Window

```bash
pt-query-digest --type slowlog --since '2026-02-27 14:00:00' --until '2026-02-27 15:00:00' /path/to/peql-slow.log
```

### Show Sequential Scans Only

The extended `# Key: Value` attributes are available in the event hash:

```bash
pt-query-digest --type slowlog --filter '$event->{Full_scan} eq "Yes"' /path/to/peql-slow.log
```

### Output to File

```bash
pt-query-digest --type slowlog /path/to/peql-slow.log > report.txt
```

### Comparing Two Time Periods

```bash
pt-query-digest --type slowlog /path/to/before.log > before.txt
pt-query-digest --type slowlog /path/to/after.log  > after.txt
diff before.txt after.txt
```

### Sampled Data

When `peql.rate_limit > 1`, each log entry includes `Log_slow_rate_type` and `Log_slow_rate_limit` metadata. pt-query-digest can use this to extrapolate totals from sampled data.

## Architecture

### Hook Chain

The extension installs hooks in `_PG_init()`, chaining with any previously installed hooks so it coexists with other extensions (e.g., `pg_stat_statements`, `auto_explain`):

```
                                        ┌──────────────────────────┐
                                        │   shared_preload_libs    │
                                        │  loads _PG_init()        │
                                        └────────────┬─────────────┘
                                                     │
             ┌───────────────────────────────────────┼───────────────────────────────────────┐
             │                                       │                                       │
    ┌────────▼──────────┐                ┌───────────▼────────────┐              ┌───────────▼───────────┐
    │  planner_hook     │                │  ExecutorStart_hook    │              │  ProcessUtility_hook  │
    │  measure planning │                │  enable INSTRUMENT_ALL │              │  time DDL statements  │
    │  time             │                └────────────┬───────────┘              └───────────────────────┘
    └───────────────────┘                             │
                                         ┌────────────▼──────────┐
                                         │  ExecutorRun_hook     │
                                         │  track nesting depth  │
                                         └────────────┬──────────┘
                                                      │
                                         ┌────────────▼──────────┐
                                         │  ExecutorFinish_hook  │
                                         │  track nesting depth  │
                                         └────────────┬──────────┘
                                                      │
                                         ┌────────────▼──────────┐
                                         │  ExecutorEnd_hook     │
                                         │  compute deltas       │
                                         │  apply rate limiter   │
                                         │  format + write entry │
                                         └────────────┬──────────┘
                                                      │
                                         ┌────────────▼──────────┐
                                         │  peql-slow.log        │
                                         └────────────┬──────────┘
                                                      │
                                         ┌────────────▼──────────┐
                                         │  pt-query-digest      │
                                         └───────────────────────┘
```

### Rate Limiting

The extension implements the Percona Server rate limiting model:

- **Session mode** (`peql.rate_limit_type = 'session'`): On first query in a backend, draw once from the PRNG. If selected (1-in-N chance), every query in this session is logged. This produces complete session traces for sampled sessions.
- **Query mode** (`peql.rate_limit_type = 'query'`): Each query independently draws from the PRNG with a 1-in-N chance of being logged. This gives a uniform sample across all sessions.
- **Always-log override**: Queries exceeding `peql.rate_limit_always_log_duration` bypass the rate limiter entirely, ensuring very slow queries are never missed.

### File I/O

- Uses PostgreSQL's `AllocateFile()`/`FreeFile()` for managed file handles
- Each backend opens, appends, and closes the file per log entry
- `O_APPEND` guarantees atomic writes on POSIX for entries under `PIPE_BUF` (typically 4-64 KB)
- If the log directory doesn't exist, the extension creates it automatically
- Log path resolution: `peql.log_directory` overrides PostgreSQL's `log_directory`; relative paths resolve against `DataDir`

### Plan Tree Analysis

At `full` verbosity, the extension walks the executed plan tree using `planstate_tree_walker` to extract:

- **Full_scan**: Any `SeqScan` node present
- **Filesort**: Any `Sort` node present
- **Filesort_on_disk**: Sort node that spilled to disk (via `TuplesortInstrumentation`)
- **Temp_table**: Any `Material` node present
- **Temp_table_on_disk**: Temp blocks written > 0
- **Rows_examined**: Sum of `ntuples` across all scan nodes (more accurate than `Rows_sent`)

## SQL Functions

### `pg_enhanced_query_logging_reset()`

Truncates the current slow query log file. Requires superuser privileges.

```sql
SELECT pg_enhanced_query_logging_reset();
-- NOTICE: peql: log file "/path/to/peql-slow.log" has been truncated
-- Returns: true
```

## Testing

The extension has three testing layers: SQL regression tests, Perl TAP tests, and a manual testing guide. All automated tests use standard PostgreSQL testing infrastructure and work on both macOS and Linux.

### Prerequisites

Beyond the build requirements (C compiler, `pg_config`), the test suites need the PostgreSQL server development files and Perl modules that ship with them. The TAP tests additionally require `IPC::Run`, which is often not installed by default.

**Ubuntu / Debian:**

```bash
sudo apt-get install postgresql-server-dev-17 libipc-run-perl
```

Replace `17` with your PostgreSQL major version (e.g., `18`). The `postgresql-server-dev-*` package provides `pg_regress`, the `PostgreSQL::Test::Cluster` and `PostgreSQL::Test::Utils` Perl modules, and the PGXS Makefile infrastructure. `libipc-run-perl` provides the `IPC::Run` module required by the TAP harness.

**Fedora / RHEL / Rocky:**

```bash
sudo dnf install postgresql17-devel perl-IPC-Run
```

**macOS (Homebrew):**

```bash
brew install postgresql@17
cpan IPC::Run        # if not already installed
```

Homebrew's `postgresql@*` formula includes the server dev files, `pg_regress`, and the Perl test modules. `IPC::Run` may need to be installed separately via `cpan` or `cpanm`.

**Verifying prerequisites:**

```bash
# Check that pg_config is available
pg_config --version

# Check that IPC::Run is installed (required for TAP tests)
perl -MIPC::Run -e 'print "ok\n"'

# Check that PostgreSQL test modules are available
perl -MPostgreSQL::Test::Cluster -e 'print "ok\n"'
```

If any of these fail, install the missing package as shown above. The SQL regression tests (`make installcheck`) only need `pg_regress` and do not require any Perl modules.

### Quick Start

Run the full automated test suite after building and installing:

```bash
# SQL regression tests (GUC defaults, extension load/unload)
make installcheck USE_PGXS=1

# TAP tests (log output, rate limiting, metrics, all features)
make prove_installcheck USE_PGXS=1
```

Or specify `pg_config` explicitly:

```bash
make installcheck USE_PGXS=1 PG_CONFIG=/path/to/pg_config
make prove_installcheck USE_PGXS=1 PG_CONFIG=/path/to/pg_config
```

### SQL Regression Tests

Three tests in `sql/` validate core SQL behavior:

| Test | What it covers |
|------|----------------|
| `01_basic` | Extension CREATE/DROP, version check, reset function |
| `02_guc` | GUC defaults and validation for all `peql.*` settings |
| `03_filtering` | Statement filtering behavior |

Expected output files live in `expected/`. A failing test produces `regression.diffs` with the mismatch.

### TAP Tests

Ten Perl TAP tests in `t/` verify actual log output using `PostgreSQL::Test::Cluster` to spin up temporary server instances. A shared helper module (`t/PeqlNode.pm`) provides common setup and log-reading utilities.

| Test | What it covers |
|------|----------------|
| `001_basic_logging` | Log file creation, pt-query-digest format, reset function, enable/disable |
| `002_rate_limiting` | Per-query sampling, rate-limit metadata, always-log-duration override |
| `003_extended_metrics` | Verbosity levels, buffer/WAL metrics, plan quality indicators, utility logging, row counts |
| `004_nested_logging` | `peql.log_nested` on/off with PL/pgSQL inner statements |
| `005_parameter_values` | Bind parameter logging, NULL handling, prepared statements |
| `006_query_plan` | EXPLAIN plan output in text and JSON format |
| `007_planning_time` | `peql.track_planning` on/off across verbosity levels |
| `008_min_duration` | Duration threshold filtering with `pg_sleep` |
| `009_session_rate_limit` | Session-mode rate limiting all-or-nothing behavior |
| `010_edge_cases` | Disk sort, disk temp table, memory tracking, schema field changes |

### Meson Build

The extension includes a `meson.build` for compatibility with PostgreSQL's Meson build system (PG 16+). When building as part of the PostgreSQL source tree:

```bash
meson setup build
cd build
meson test --suite pg_enhanced_query_logging
```

### CI/CD

A GitHub Actions workflow (`.github/workflows/test.yml`) runs both test suites automatically on every push and pull request. The CI matrix covers:

- **Platforms**: Ubuntu and macOS
- **PostgreSQL versions**: 17 and 18
- **Test suites**: pg_regress (SQL) and TAP (Perl)

Test artifacts (diffs, logs) are uploaded on failure for debugging.

### Sample Configuration

A sample configuration file is included at `pg_enhanced_query_logging.conf`. You can include it in `postgresql.conf`:

```
include = '/path/to/pg_enhanced_query_logging.conf'
```

### Manual Testing

See `test/TESTING.md` for a comprehensive manual testing guide covering all features with step-by-step instructions and expected output. The manual guide complements the automated tests and is useful for exploratory testing and validating output with `pt-query-digest`.

## Compatibility Notes

- **Lock_time** is always reported as `0.000000`. PostgreSQL does not expose per-query lock acquisition time in the same way MySQL does. The field is present for pt-query-digest format compatibility.
- **Rows_examined** at `full` verbosity reflects actual tuples processed by scan nodes in the plan tree. At lower verbosity levels it falls back to `Rows_sent` for SELECTs.
- **Buffer and WAL metrics** are per-query executor deltas (zero-initialized per query via `InstrAlloc`). Buffer I/O during `ExecutorStart` (e.g., catalog lookups for plan initialization) is not captured, matching the behavior of `auto_explain` and `pg_stat_statements`. The practical impact is minimal.
- **I/O timing metrics** require PostgreSQL's `track_io_timing = on` to be set at the server level for the underlying counters to be populated.
- **Memory tracking** (`peql.track_memory`) is experimental and adds overhead. It measures total bytes allocated in the query's memory context.
- **Nested query timing**: When `peql.log_nested = on`, nested queries inside PL/pgSQL functions are logged individually. The enclosing query's `Query_time` includes time spent in nested queries, so the same execution time may appear in multiple log entries. pt-query-digest reports will show inflated total time when nested logging is enabled. To avoid double-counting, use `peql.log_nested = off` (the default) or post-process the log to exclude nested entries.
- The extension coexists with `pg_stat_statements`, `auto_explain`, and other hook-based extensions through proper hook chaining.

## Contributing

Contributions are welcome. Here's how to get started:

1. Fork the repository and create a feature branch
2. Make your changes, following the existing code style (PostgreSQL C conventions)
3. Add or update tests as appropriate:
   - SQL regression tests in `sql/` with expected output in `expected/`
   - TAP tests in `t/` for log output verification (use `t/PeqlNode.pm` for common setup)
4. Ensure the extension compiles cleanly: `make USE_PGXS=1`
5. Run the full test suite: `make installcheck USE_PGXS=1 && make prove_installcheck USE_PGXS=1`
6. Submit a pull request -- CI will run both test suites automatically on Ubuntu and macOS

### Code Style

- Follow [PostgreSQL coding conventions](https://www.postgresql.org/docs/current/source-format.html): tabs for indentation, K&R brace style, descriptive variable names
- Use `ereport()` / `elog()` for error reporting
- Use `AllocateFile()` / `FreeFile()` instead of raw `fopen()` / `fclose()`
- Prefix all symbols with `peql_` to avoid namespace collisions

## License

This project is released under the [PostgreSQL License](https://opensource.org/licenses/PostgreSQL), the same license used by PostgreSQL itself.

## Acknowledgments

- Inspired by [Percona Server's extended slow query log](https://docs.percona.com/percona-server/innovation-release/slow-extended.html)
- Built for use with [Percona Toolkit's pt-query-digest](https://docs.percona.com/percona-toolkit/pt-query-digest.html)
