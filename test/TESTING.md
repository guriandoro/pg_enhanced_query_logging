# Testing pg_enhanced_query_logging

Manual testing guide for the pg_enhanced_query_logging extension against
PostgreSQL 18 installed at `/opt/postgresql/18.3`.

## Prerequisites

- PostgreSQL 18 installed at `/opt/postgresql/18.3`
- The extension source compiled (see Build step below)
- Optionally, `pt-query-digest` from Percona Toolkit for format validation

## 1. Build and Install

```bash
cd /Users/agustin/src/guriandoro/pg_enhanced_query_logging

# Build
make USE_PGXS=1 PG_CONFIG=/opt/postgresql/18.3/bin/pg_config

# Install (requires write access to the PG installation directory)
sudo make install USE_PGXS=1 PG_CONFIG=/opt/postgresql/18.3/bin/pg_config
```

This copies `pg_enhanced_query_logging.dylib` into `/opt/postgresql/18.3/lib/`
and the `.control` + `.sql` files into `/opt/postgresql/18.3/share/extension/`.

## 2. Create a Test Cluster

```bash
export PGDATA=/tmp/peql_test
export PATH=/opt/postgresql/18.3/bin:$PATH

initdb -D $PGDATA
```

## 3. Configure the Extension

Append the following to `$PGDATA/postgresql.conf`:

```
# --- pg_enhanced_query_logging test config ---
shared_preload_libraries = 'pg_enhanced_query_logging'

# Extension settings
peql.enabled               = on
peql.log_min_duration      = 0          # log ALL queries for testing
peql.log_verbosity         = 'full'     # emit all extended metrics
peql.log_utility           = on         # log DDL statements
peql.log_nested            = off        # off initially; toggled in tests
peql.track_planning        = on         # show Plan_time
peql.track_wal             = on         # show WAL_records / WAL_bytes / WAL_fpi
peql.track_io_timing       = on         # show Shared_blk_read_time / write_time
peql.track_memory          = on         # show Mem_allocated
peql.log_parameter_values  = off        # off initially; toggled in tests
peql.log_query_plan        = off        # off initially; toggled in tests

# Rate limiting (disabled for initial tests)
peql.rate_limit            = 1
peql.rate_limit_type       = 'query'

# PostgreSQL settings required for full metrics
logging_collector          = on
log_directory              = 'log'
track_io_timing            = on         # PG-level I/O counter prerequisite
```

## 4. Start the Server

```bash
pg_ctl -D $PGDATA -l $PGDATA/logfile start
```

Verify the extension loaded by checking the server log:

```bash
grep -i "peql\|enhanced_query" $PGDATA/logfile
```

## 5. Create the Extension

```bash
psql -d postgres -c "CREATE EXTENSION pg_enhanced_query_logging;"
```

## 6. Core Logging Tests

### 6.1 Basic SELECT (Full_scan: Yes)

```bash
psql -d postgres -c "SELECT generate_series(1, 10000);"
```

**Expected:** log entry with `Full_scan: Yes`, `Filesort: No`,
`Rows_sent: 10000`, `Rows_examined` close to 10000.

### 6.2 SELECT with ORDER BY (Filesort: Yes)

```bash
psql -d postgres -c "SELECT generate_series(1, 10000) AS n ORDER BY n DESC;"
```

**Expected:** `Filesort: Yes`, `Full_scan: Yes`.

### 6.3 DML -- CREATE TABLE AS, UPDATE, DELETE (Rows_affected)

```bash
psql -d postgres <<'SQL'
CREATE TABLE test_tbl AS SELECT generate_series(1, 1000) AS id;
UPDATE test_tbl SET id = id + 1;
DELETE FROM test_tbl WHERE id < 500;
SQL
```

**Expected:** `Rows_affected` appears on the `UPDATE` and `DELETE` entries.
Buffer counters (`Shared_blks_hit`, `Shared_blks_read`, etc.) should show
nonzero values for operations touching real heap pages.

### 6.4 Query against a real table (buffer metrics)

```bash
psql -d postgres <<'SQL'
CREATE TABLE big_tbl AS SELECT generate_series(1, 100000) AS id, md5(random()::text) AS data;
ANALYZE big_tbl;
SELECT count(*) FROM big_tbl WHERE id > 50000;
SQL
```

**Expected:** `Shared_blks_hit` and/or `Shared_blks_read` > 0.
`WAL_records` and `WAL_bytes` > 0 on the `CREATE TABLE` and `ANALYZE`.

### 6.5 Heavy query to trigger JIT (if available)

```bash
psql -d postgres -c "SET jit_above_cost = 10; SELECT count(*), sum(id), avg(id) FROM generate_series(1, 1000000) AS id;"
```

**Expected:** if JIT fires, the log entry will include `JIT_functions`,
`JIT_generation_time`, `JIT_inlining_time`, `JIT_optimization_time`, and
`JIT_emission_time`. If JIT is not compiled in or the cost threshold isn't
met, these lines simply won't appear.

### 6.6 Schema field (db.schema format)

```bash
psql -d postgres -c "SET search_path = pg_catalog; SELECT 1;"
psql -d postgres -c "SET search_path = public; SELECT 1;"
```

**Expected:** the `Schema` field changes between `postgres.pg_catalog` and
`postgres.public` across the two entries.

## 7. Inspect the Log Output

```bash
cat $PGDATA/log/peql-slow.log
```

A full-verbosity entry looks like:

```
# Time: 2026-02-27T23:10:00.123456
# User@Host: agustin[agustin] @ [local] []
# Thread_id: 12345  Schema: postgres.public
# Query_time: 0.052311  Lock_time: 0.000000  Rows_sent: 10000  Rows_examined: 10000
# Shared_blks_hit: 0  Shared_blks_read: 0  Shared_blks_dirtied: 0  Shared_blks_written: 0
# Local_blks_hit: 0  Local_blks_read: 0  Local_blks_written: 0
# Temp_blks_read: 0  Temp_blks_written: 0
# Shared_blk_read_time: 0.000000  Shared_blk_write_time: 0.000000
# WAL_records: 0  WAL_bytes: 0  WAL_fpi: 0
# Plan_time: 0.000250
# Full_scan: Yes  Temp_table: No  Temp_table_on_disk: No  Filesort: No  Filesort_on_disk: No
# Mem_allocated: 32768
SET timestamp=1772147400;
SELECT generate_series(1, 10000);
```

## 8. Validate with pt-query-digest

If `pt-query-digest` is installed:

```bash
pt-query-digest --type slowlog $PGDATA/log/peql-slow.log
```

The tool should parse all entries without errors. Extended `# Key: Value`
lines that pt-query-digest doesn't recognise natively are safely ignored
(its parser accepts any `# Word: value` pair).

## 9. Test GUC Runtime Changes

GUC variables marked `PGC_SUSET` can be changed per-session by superusers:

```bash
# Switch to minimal verbosity (only timing + rows)
psql -d postgres -c "SET peql.log_verbosity = 'minimal'; SELECT 1;"

# Switch to standard verbosity (adds Thread_id, Schema)
psql -d postgres -c "SET peql.log_verbosity = 'standard'; SELECT 1;"

# Disable logging temporarily
psql -d postgres -c "SET peql.log_min_duration = -1; SELECT 1;"

# Only log queries slower than 500ms
psql -d postgres -c "SET peql.log_min_duration = 500; SELECT pg_sleep(1);"

# Turn off plan-tree analysis overhead
psql -d postgres -c "SET peql.log_verbosity = 'standard'; SELECT 1;"
```

After each command, check the log file to verify that verbosity changes
take effect. Entries logged at `minimal` should have no buffer/WAL/JIT lines.

## 10. Test the Reset Function

```bash
# Verify the file has content
wc -l $PGDATA/log/peql-slow.log

# Truncate it
psql -d postgres -c "SELECT pg_enhanced_query_logging_reset();"

# Confirm it's empty
wc -l $PGDATA/log/peql-slow.log
```

## 11. Test Rate Limiting

### 11.1 Query-mode rate limiting

```bash
psql -d postgres -c "SELECT pg_enhanced_query_logging_reset();"

# With rate_limit=1, all queries are logged
psql -d postgres <<'SQL'
SET peql.rate_limit = 1;
SELECT 'rate_one_a';
SELECT 'rate_one_b';
SELECT 'rate_one_c';
SQL

wc -l $PGDATA/log/peql-slow.log
```

**Expected:** at least 3 `# Time:` lines (one per query).

```bash
psql -d postgres -c "SELECT pg_enhanced_query_logging_reset();"

# With rate_limit=1000, very few queries should be logged
psql -d postgres <<'SQL'
SET peql.rate_limit = 1000;
SET peql.rate_limit_type = 'query';
SELECT generate_series(1, 200);
SQL
```

**Expected:** significantly fewer than 200 `# Time:` lines.

### 11.2 Rate limit metadata in output

```bash
psql -d postgres -c "SELECT pg_enhanced_query_logging_reset();"
psql -d postgres <<'SQL'
SET peql.rate_limit = 2;
SET peql.rate_limit_type = 'query';
SELECT 1; SELECT 2; SELECT 3; SELECT 4; SELECT 5;
SELECT 6; SELECT 7; SELECT 8; SELECT 9; SELECT 10;
SQL
```

**Expected:** logged entries contain
`# Log_slow_rate_type: query  Log_slow_rate_limit: 2`.

```bash
grep "Log_slow_rate_type" $PGDATA/log/peql-slow.log
```

### 11.3 No rate limit metadata when rate_limit=1

```bash
psql -d postgres -c "SELECT pg_enhanced_query_logging_reset();"
psql -d postgres -c "SET peql.rate_limit = 1; SELECT 'no_rate_meta';"
grep "Log_slow_rate_type" $PGDATA/log/peql-slow.log
```

**Expected:** no output from grep (metadata is omitted when not sampling).

### 11.4 Always-log-duration bypasses rate limiter

```bash
psql -d postgres -c "SELECT pg_enhanced_query_logging_reset();"
psql -d postgres <<'SQL'
SET peql.rate_limit = 1000000;
SET peql.rate_limit_always_log_duration = 0;
SELECT pg_sleep(0.01);
SQL
```

**Expected:** the `pg_sleep` query appears in the log despite the extreme
rate limit, because `always_log_duration = 0` means any query (duration >= 0)
bypasses the limiter.

**Known issue:** setting `rate_limit_always_log_duration = 0` currently does
NOT trigger the always-log path due to a `> 0` guard in the code (see
`bugs_and_improvements/21_*`). The query may still appear if it passes the
normal rate limiter draw. Test will need updating after the fix.

### 11.5 Session-mode rate limiting

```bash
psql -d postgres -c "SELECT pg_enhanced_query_logging_reset();"
psql -d postgres <<'SQL'
SET peql.rate_limit = 2;
SET peql.rate_limit_type = 'session';
SELECT 'session_test_1';
SELECT 'session_test_2';
SELECT 'session_test_3';
SQL
```

**Expected:** either all three queries are logged (this session was sampled)
or none are logged (this session was not sampled). There should be no mix of
logged/not-logged within the same session.

## 12. Test Utility Statement Logging

### 12.1 DDL statements with log_utility = on

```bash
psql -d postgres -c "SELECT pg_enhanced_query_logging_reset();"
psql -d postgres <<'SQL'
SET peql.log_utility = on;
CREATE TABLE util_test (id int);
ALTER TABLE util_test ADD COLUMN name text;
DROP TABLE util_test;
SQL

grep -E "CREATE TABLE|ALTER TABLE|DROP TABLE" $PGDATA/log/peql-slow.log
```

**Expected:** all three DDL statements appear in the log.

### 12.2 DDL statements with log_utility = off

```bash
psql -d postgres -c "SELECT pg_enhanced_query_logging_reset();"
psql -d postgres <<'SQL'
SET peql.log_utility = off;
CREATE TABLE util_test_off (id int);
DROP TABLE util_test_off;
SQL

grep "util_test_off" $PGDATA/log/peql-slow.log
```

**Expected:** no output (DDL statements are not logged).

## 13. Test Nested Statement Logging

### 13.1 PL/pgSQL function with log_nested = off (default)

```bash
psql -d postgres <<'SQL'
CREATE OR REPLACE FUNCTION nested_test() RETURNS void AS $$
BEGIN
  PERFORM 1;
  PERFORM 2;
  PERFORM 3;
END;
$$ LANGUAGE plpgsql;
SQL

psql -d postgres -c "SELECT pg_enhanced_query_logging_reset();"
psql -d postgres <<'SQL'
SET peql.log_nested = off;
SELECT nested_test();
SQL

grep "PERFORM\|SELECT 1\|SELECT 2\|SELECT 3" $PGDATA/log/peql-slow.log
```

**Expected:** only the outer `SELECT nested_test()` call is logged.
The inner `PERFORM` statements do NOT appear.

### 13.2 PL/pgSQL function with log_nested = on

```bash
psql -d postgres -c "SELECT pg_enhanced_query_logging_reset();"
psql -d postgres <<'SQL'
SET peql.log_nested = on;
SELECT nested_test();
SQL

cat $PGDATA/log/peql-slow.log
```

**Expected:** both the outer call and the inner statements appear as
separate log entries. Note that the outer query's `Query_time` includes
the time of the inner statements (this is expected double-counting;
see `bugs_and_improvements/24_*`).

## 14. Test Parameter Value Logging

### 14.1 Prepared statements with log_parameter_values = on

```bash
psql -d postgres -c "SELECT pg_enhanced_query_logging_reset();"
psql -d postgres <<'SQL'
SET peql.log_parameter_values = on;
PREPARE param_test (int, text) AS SELECT $1, $2;
EXECUTE param_test(42, 'hello world');
DEALLOCATE param_test;
SQL

grep "Parameters" $PGDATA/log/peql-slow.log
```

**Expected:** a line like `# Parameters: $1 = '42', $2 = 'hello world'`
appears after the query text.

### 14.2 NULL parameter values

```bash
psql -d postgres -c "SELECT pg_enhanced_query_logging_reset();"
psql -d postgres <<'SQL'
SET peql.log_parameter_values = on;
PREPARE null_test (int, text) AS SELECT $1, $2;
EXECUTE null_test(NULL, NULL);
DEALLOCATE null_test;
SQL

grep "Parameters" $PGDATA/log/peql-slow.log
```

**Expected:** `# Parameters: $1 = NULL, $2 = NULL`.

### 14.3 Parameters not logged when log_parameter_values = off

```bash
psql -d postgres -c "SELECT pg_enhanced_query_logging_reset();"
psql -d postgres <<'SQL'
SET peql.log_parameter_values = off;
PREPARE noparams (int) AS SELECT $1;
EXECUTE noparams(99);
DEALLOCATE noparams;
SQL

grep "Parameters" $PGDATA/log/peql-slow.log
```

**Expected:** no output (parameter line is not emitted).

## 15. Test EXPLAIN Plan Output

### 15.1 Text-format plan with log_query_plan = on

```bash
psql -d postgres -c "SELECT pg_enhanced_query_logging_reset();"
psql -d postgres <<'SQL'
SET peql.log_query_plan = on;
SET peql.log_query_plan_format = 'text';
SELECT count(*) FROM big_tbl WHERE id > 50000;
SQL

grep -A 5 "# Plan:" $PGDATA/log/peql-slow.log
```

**Expected:** a `# Plan:` line followed by EXPLAIN ANALYZE output showing
node types, actual rows, and timing.

### 15.2 JSON-format plan

```bash
psql -d postgres -c "SELECT pg_enhanced_query_logging_reset();"
psql -d postgres <<'SQL'
SET peql.log_query_plan = on;
SET peql.log_query_plan_format = 'json';
SELECT count(*) FROM big_tbl WHERE id > 50000;
SQL

grep -A 10 "# Plan:" $PGDATA/log/peql-slow.log
```

**Expected:** EXPLAIN output in JSON format after the `# Plan:` line.

### 15.3 Plan not logged when log_query_plan = off

```bash
psql -d postgres -c "SELECT pg_enhanced_query_logging_reset();"
psql -d postgres <<'SQL'
SET peql.log_query_plan = off;
SELECT count(*) FROM big_tbl WHERE id > 50000;
SQL

grep "Plan:" $PGDATA/log/peql-slow.log
```

**Expected:** no output (plan is not included).

## 16. Test Enable/Disable Toggle

### 16.1 peql.enabled = off disables all logging

```bash
psql -d postgres -c "SELECT pg_enhanced_query_logging_reset();"
psql -d postgres -c "SET peql.enabled = off; SELECT 'should_not_appear';"
grep "should_not_appear" $PGDATA/log/peql-slow.log
```

**Expected:** no output.

### 16.2 peql.log_min_duration = -1 disables logging

```bash
psql -d postgres -c "SELECT pg_enhanced_query_logging_reset();"
psql -d postgres -c "SET peql.log_min_duration = '-1'; SELECT 'also_hidden';"
grep "also_hidden" $PGDATA/log/peql-slow.log
```

**Expected:** no output.

### 16.3 peql.log_min_duration threshold filtering

```bash
psql -d postgres -c "SELECT pg_enhanced_query_logging_reset();"
psql -d postgres <<'SQL'
SET peql.log_min_duration = 500;
SELECT 'fast_query';
SELECT pg_sleep(0.6);
SQL

grep -c "# Time:" $PGDATA/log/peql-slow.log
```

**Expected:** only 1 entry (the `pg_sleep` query). The fast `SELECT` is
below the 500ms threshold and is not logged.

## 17. Test Planning Time Tracking

```bash
psql -d postgres -c "SELECT pg_enhanced_query_logging_reset();"
psql -d postgres <<'SQL'
SET peql.log_verbosity = 'full';
SET peql.track_planning = on;
SELECT count(*) FROM big_tbl WHERE id > 50000;
SQL

grep "Plan_time" $PGDATA/log/peql-slow.log
```

**Expected:** `# Plan_time:` line with a nonzero value.

```bash
psql -d postgres -c "SELECT pg_enhanced_query_logging_reset();"
psql -d postgres <<'SQL'
SET peql.log_verbosity = 'full';
SET peql.track_planning = off;
SELECT count(*) FROM big_tbl WHERE id > 50000;
SQL

grep "Plan_time" $PGDATA/log/peql-slow.log
```

**Expected:** no `Plan_time` line.

## 18. Verification Checklist

| Check                                  | How to verify                                                         |
| -------------------------------------- | --------------------------------------------------------------------- |
| Extension loads without errors         | No errors in `$PGDATA/logfile` on startup                             |
| Queries are logged                     | Entries appear in `$PGDATA/log/peql-slow.log`                         |
| `peql.enabled = off` disables logging  | No entries appear with master switch off                              |
| `peql.log_min_duration = -1` disables  | No entries appear with duration set to -1                             |
| `peql.log_min_duration` filters        | Only slow queries appear when threshold is set                        |
| `Schema` shows `db.schema`            | e.g. `postgres.public` not just `postgres`                            |
| `Rows_examined` uses plan-tree data    | Value may differ from `Rows_sent` on filtered queries                 |
| `Rows_affected` on DML                 | Nonzero on UPDATE/DELETE at standard+ verbosity                       |
| Buffer counters are populated          | Nonzero `Shared_blks_*` for table scans                               |
| WAL counters are populated             | Nonzero `WAL_records` on INSERT/UPDATE/DELETE                         |
| I/O timing appears                     | `Shared_blk_read_time` present when `track_io_timing = on`           |
| `Plan_time` appears                    | Present when `peql.track_planning = on`                               |
| `Full_scan: Yes` on SeqScan           | Any `SELECT` without an index                                         |
| `Filesort: Yes` on ORDER BY           | Queries with `ORDER BY`                                               |
| `Filesort_on_disk: Yes`               | Large sorts exceeding `work_mem` (set `work_mem = '64kB'` to force)   |
| `Temp_table: Yes` on Material nodes   | Queries with CTEs or subqueries that materialize                      |
| JIT metrics appear                     | Lower `jit_above_cost` and run an expensive aggregate                 |
| `Mem_allocated` appears                | When `peql.track_memory = on`                                         |
| Verbosity levels work                  | `minimal` < `standard` < `full` produce progressively more fields     |
| Rate limiting reduces entries          | `rate_limit=1000` logs far fewer than N out of N queries              |
| Rate limit metadata in output          | `Log_slow_rate_type` / `Log_slow_rate_limit` present when sampling    |
| No rate metadata when rate_limit=1     | `Log_slow_rate_type` absent when not sampling                         |
| Session-mode rate limiting             | All-or-nothing logging per session                                    |
| Always-log-duration override           | Very slow queries logged despite high rate_limit                      |
| Utility logging (DDL)                  | DDL appears when `peql.log_utility = on`, absent when off             |
| Nested statement logging               | Inner function statements appear when `peql.log_nested = on`          |
| Parameter value logging                | `# Parameters:` line present when `peql.log_parameter_values = on`    |
| NULL parameters handled                | NULL params shown as `NULL` not as a crash                            |
| EXPLAIN plan in text format            | `# Plan:` with EXPLAIN output when `peql.log_query_plan = on`        |
| EXPLAIN plan in JSON format            | JSON EXPLAIN output with `peql.log_query_plan_format = 'json'`        |
| Reset function truncates the file      | `pg_enhanced_query_logging_reset()` empties the file                  |
| pt-query-digest can parse the output   | `pt-query-digest --type slowlog` produces a report without errors     |

## 19. Forcing Edge Cases

```bash
# Force disk-based sort (Filesort_on_disk: Yes)
psql -d postgres -c "SET work_mem = '64kB'; SELECT * FROM generate_series(1, 100000) AS n ORDER BY n DESC;"

# Force temp-table on disk (Temp_table_on_disk: Yes)
psql -d postgres -c "SET work_mem = '64kB'; WITH big AS (SELECT generate_series(1, 100000) AS n) SELECT count(*) FROM big;"
```

## 20. Automated Tests

In addition to the manual tests above, the extension includes automated test
suites.

### 20.1 SQL Regression Tests

Validate GUC behavior, extension loading/unloading, and the reset function:

```bash
make installcheck USE_PGXS=1 PG_CONFIG=/opt/postgresql/18.3/bin/pg_config
```

Test files: `sql/01_basic.sql`, `sql/02_guc.sql`, `sql/03_filtering.sql`
Expected outputs: `expected/01_basic.out`, `expected/02_guc.out`, `expected/03_filtering.out`

### 20.2 TAP Tests

Verify log file creation, output format, rate limiting behavior, verbosity
levels, buffer/WAL metrics, utility logging, and row count accuracy:

```bash
make prove_installcheck USE_PGXS=1 PG_CONFIG=/opt/postgresql/18.3/bin/pg_config
```

Test files:
- `t/001_basic_logging.pl` -- log file creation, pt-query-digest format, reset function, enable/disable
- `t/002_rate_limiting.pl` -- rate limit sampling, metadata output, always-log override
- `t/003_extended_metrics.pl` -- verbosity levels, buffer/WAL metrics, plan quality indicators, utility logging, row counts

## 21. Clean Up

```bash
pg_ctl -D $PGDATA stop
rm -rf /tmp/peql_test
```
