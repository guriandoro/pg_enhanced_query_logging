# Testing pg_enhanced_query_logging

Manual testing guide for the pg_enhanced_query_logging extension against
PostgreSQL 18 installed at `/opt/postgresql/18.3`.

## Prerequisites

- PostgreSQL 18 installed at `/opt/postgresql/18.3`
- The extension source compiled (see Build step below)
- Optionally, `pt-query-digest` from Percona Toolkit for format validation
- For the TAP (Test Anything Protocol) tests: `IPC::Run` Perl module and the PostgreSQL test Perl modules

### Installing IPC::Run on macOS

The TAP tests require the `IPC::Run` Perl module, which is not included with the macOS system Perl. Install it with one of:

```bash
# Option A: cpan (installs to system Perl paths, requires sudo)
sudo cpan IPC::Run

# Option B: cpanm (installs to ~/perl5, no root required)
brew install cpanminus
cpanm IPC::Run
```

If you used `cpanm` (Option B), configure your shell to find the module:

```bash
eval $(perl -I ~/perl5/lib/perl5/ -Mlocal::lib)
```

To make this permanent, add that line to your `~/.zshrc`.

### PostgreSQL test Perl modules

The TAP tests use `PostgreSQL::Test::Cluster` and `PostgreSQL::Test::Utils`.
When PostgreSQL is built from source, these modules live in the source tree
(not in the installed prefix). Pass the source path to `prove` with `-I`:

```bash
prove -I /Users/agustin/src/postgres/src/test/perl t/*.pl
```

**Important:** The source tree version must match the installed PostgreSQL
binaries. If the installed version is 18.3, checkout `REL_18_3` in the
source tree before running the tests. A version mismatch (e.g., development
HEAD vs release binaries) can cause API incompatibilities in the test Perl
modules that manifest as cryptic errors like
`Use of uninitialized value $_[0] in join or string`.

```bash
cd /Users/agustin/src/postgres
git checkout REL_18_3
```

### PG_REGRESS environment variable

The `PostgreSQL::Test::Cluster` module requires the `PG_REGRESS`
environment variable to point to the `pg_regress` binary. This is set
automatically when running tests via `make`, but must be set manually
when using `prove` directly:

```bash
export PG_REGRESS=/opt/postgresql/18.3/lib/pgxs/src/test/regress/pg_regress
```

Without this, tests will fail during `$node->init` with
`Use of uninitialized value` errors.

### Verify all prerequisites

```bash
perl -MIPC::Run -e 'print "IPC::Run OK\n"'
perl -I /Users/agustin/src/postgres/src/test/perl -MPostgreSQL::Test::Cluster -e 'print "PG test modules OK\n"'
```

### Running the TAP tests

The complete command with all required environment variables:

```bash
# Set up the environment
eval $(perl -I ~/perl5/lib/perl5/ -Mlocal::lib)  # only if IPC::Run installed via cpanm
export PATH=/opt/postgresql/18.3/bin:$PATH
export PG_REGRESS=/opt/postgresql/18.3/lib/pgxs/src/test/regress/pg_regress

# Run all TAP tests
prove -v -I /Users/agustin/src/postgres/src/test/perl t/*.pl
```

### Troubleshooting

**Stale `tmp_check` directory:** If a previous test run crashed or was
interrupted, `prove` leaves behind a `tmp_check/` directory containing the
test cluster data. Subsequent runs will fail with
`could not create data directory ... File exists`. Remove it before
retrying:

```bash
rm -rf tmp_check
```

**`Use of uninitialized value $_[0]` during `$node->init`:** This usually
means `PG_REGRESS` is not set. See [PG_REGRESS environment variable](#pg_regress-environment-variable) above.

**`Can't locate IPC/Run.pm`:** The `IPC::Run` Perl module is not
installed or not in Perl's `@INC` path. See [Installing IPC::Run on macOS](#installing-ipcrun-on-macos).

**`02_guc` SQL regression test fails:** The test expects default `peql.*`
GUC (Grand Unified Configuration) values. If you have customized any `peql.*` settings in
`postgresql.conf`, the `SHOW` output will not match `expected/02_guc.out`.
Run against a server with default settings, or focus on the TAP tests which
create isolated instances.

## 1. Build and Install

```bash
cd /Users/agustin/src/guriandoro/pg_enhanced_query_logging

# Build
make USE_PGXS=1 PG_CONFIG=/opt/postgresql/18.3/bin/pg_config

# Install (requires write access to the PG installation directory)
make install USE_PGXS=1 PG_CONFIG=/opt/postgresql/18.3/bin/pg_config
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
peql.log_utility           = on         # log DDL (Data Definition Language) statements
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

Verify the extension loaded by checking the shared_preload_libraries variable:

```bash
psql -d postgres -c "SHOW shared_preload_libraries;"
```

## 5. Create the Extension

```bash
psql -d postgres -c "CREATE EXTENSION pg_enhanced_query_logging;"
```

## 6. Core Logging Tests

### 6.1 Basic SELECT (Full_scan: Yes)

```bash
psql -d postgres -c "SELECT generate_series(1, 10000);" > /dev/null
```

**Expected:** log entry with `Full_scan: Yes`, `Filesort: No`,
`Rows_sent: 10000`, `Rows_examined` close to 10000.

### 6.2 SELECT with ORDER BY (Filesort: Yes)

```bash
psql -d postgres -c "SELECT generate_series(1, 10000) AS n ORDER BY n DESC;" > /dev/null
```

**Expected:** `Filesort: Yes`, `Full_scan: Yes`.

### 6.3 DML (Data Manipulation Language) -- CREATE TABLE AS, UPDATE, DELETE (Rows_affected)

```bash
psql -d postgres > /dev/null <<'SQL'
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
psql -d postgres > /dev/null <<'SQL'
CREATE TABLE big_tbl AS SELECT generate_series(1, 100000) AS id, md5(random()::text) AS data;
ANALYZE big_tbl;
SELECT count(*) FROM big_tbl WHERE id > 50000;
SQL
```

**Expected:** `Shared_blks_hit` and/or `Shared_blks_read` > 0.
`WAL_records` and `WAL_bytes` > 0 on the `CREATE TABLE` and `ANALYZE`.

### 6.5 Heavy query to trigger JIT (Just-In-Time compilation), if available

```bash
psql -d postgres -c "SET jit_above_cost = 10; SELECT count(*), sum(id), avg(id) FROM generate_series(1, 1000000) AS id;" > /dev/null
```

**Expected:** if JIT fires, the log entry will include `JIT_functions`,
`JIT_generation_time`, `JIT_inlining_time`, `JIT_optimization_time`, and
`JIT_emission_time`. If JIT is not compiled in or the cost threshold isn't
met, these lines simply won't appear.

### 6.6 Schema field (db.schema format)

```bash
psql -d postgres -c "SET search_path = pg_catalog; SELECT 1;" > /dev/null
psql -d postgres -c "SET search_path = public; SELECT 1;" > /dev/null
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
psql -d postgres -c "SET peql.log_verbosity = 'minimal'; SELECT 1;" > /dev/null

# Switch to standard verbosity (adds Thread_id, Schema)
psql -d postgres -c "SET peql.log_verbosity = 'standard'; SELECT 1;" > /dev/null

# Disable logging temporarily
psql -d postgres -c "SET peql.log_min_duration = -1; SELECT 1;" > /dev/null

# Only log queries slower than 500ms
psql -d postgres -c "SET peql.log_min_duration = 500; SELECT pg_sleep(1);" > /dev/null

# Turn off plan-tree analysis overhead
psql -d postgres -c "SET peql.log_verbosity = 'standard'; SELECT 1;" > /dev/null
```

After each command, check the log file to verify that verbosity changes
take effect. Entries logged at `minimal` should have no buffer, WAL (Write-Ahead Log), or JIT lines.

## 10. Test the Reset Function

```bash
# Verify the file has content
wc -l $PGDATA/log/peql-slow.log

# Truncate it
psql -d postgres -c "SELECT pg_enhanced_query_logging_reset();" > /dev/null

# Confirm it's empty
wc -l $PGDATA/log/peql-slow.log
```

## 11. Test Rate Limiting

### 11.1 Query-mode rate limiting

```bash
psql -d postgres -c "SELECT pg_enhanced_query_logging_reset();" > /dev/null

# With rate_limit=1, all queries are logged
psql -d postgres > /dev/null <<'SQL'
SET peql.rate_limit = 1;
SELECT 'rate_one_a';
SELECT 'rate_one_b';
SELECT 'rate_one_c';
SQL

grep -c '# Time:' $PGDATA/log/peql-slow.log
```

**Expected:** at least 3 `# Time:` lines (one per query).

```bash
psql -d postgres -c "SELECT pg_enhanced_query_logging_reset();" > /dev/null

# With rate_limit=1000, very few queries should be logged
(echo "SET peql.rate_limit = 1000;";
 echo "SET peql.rate_limit_type = 'query';";
 for i in $(seq 1 2000); do echo "SELECT $i;"; done) | psql -d postgres > /dev/null
```

**Expected:** significantly fewer than 2000 `# Time:` lines.

### 11.2 Rate limit metadata in output

```bash
psql -d postgres -c "SELECT pg_enhanced_query_logging_reset();" > /dev/null
psql -d postgres > /dev/null <<'SQL'
SET peql.rate_limit = 2;
SET peql.rate_limit_type = 'query';
SELECT 1; SELECT 2; SELECT 3; SELECT 4; SELECT 5;
SELECT 6; SELECT 7; SELECT 8; SELECT 9; SELECT 10;
SQL
```

**Expected:** logged entries contain all three rate-limit metadata fields:
`# Log_slow_rate_type: query  Log_slow_rate_limit: 2  Log_slow_rate_limit_always_log_duration: 10000`

The `always_log_duration` defaults to 10000 (ms) when not explicitly changed.

```bash
grep "Log_slow_rate_type" $PGDATA/log/peql-slow.log
grep "Log_slow_rate_limit_always_log_duration" $PGDATA/log/peql-slow.log
```

### 11.3 No rate limit metadata when rate_limit=1

```bash
psql -d postgres -c "SELECT pg_enhanced_query_logging_reset();" > /dev/null
psql -d postgres -c "SET peql.rate_limit = 1; SELECT 'no_rate_meta';" > /dev/null
grep "Log_slow_rate_type" $PGDATA/log/peql-slow.log
```

**Expected:** no output from grep (metadata is omitted when not sampling).

### 11.4 Always-log-duration bypasses rate limiter

```bash
psql -d postgres -c "SELECT pg_enhanced_query_logging_reset();" > /dev/null
psql -d postgres > /dev/null <<'SQL'
SET peql.rate_limit = 1000000;
SET peql.rate_limit_always_log_duration = 0;
SELECT pg_sleep(0.01);
SQL
```

**Expected:** the `pg_sleep` query appears in the log despite the extreme
rate limit, because `always_log_duration = 0` means any query (duration >= 0)
bypasses the limiter.

### 11.5 Always-log-duration value in metadata

```bash
psql -d postgres -c "SELECT pg_enhanced_query_logging_reset();" > /dev/null
psql -d postgres > /dev/null <<'SQL'
SET peql.rate_limit = 5;
SET peql.rate_limit_always_log_duration = 2000;
SELECT 1; SELECT 2; SELECT 3; SELECT 4; SELECT 5;
SELECT 6; SELECT 7; SELECT 8; SELECT 9; SELECT 10;
SQL

grep "Log_slow_rate_limit_always_log_duration" $PGDATA/log/peql-slow.log
```

**Expected:** logged entries show
`Log_slow_rate_limit_always_log_duration: 2000`, reflecting the custom
threshold set for this session.

### 11.6 Always-log-duration disabled (-1) in metadata

```bash
psql -d postgres -c "SELECT pg_enhanced_query_logging_reset();" > /dev/null
psql -d postgres > /dev/null <<'SQL'
SET peql.rate_limit = 3;
SET peql.rate_limit_always_log_duration = -1;
SELECT 1; SELECT 2; SELECT 3; SELECT 4; SELECT 5;
SQL

grep "Log_slow_rate_limit_always_log_duration" $PGDATA/log/peql-slow.log
```

**Expected:** logged entries show
`Log_slow_rate_limit_always_log_duration: -1`, indicating the bypass
feature is disabled.

### 11.7 Auto rate limit metadata (max queries)

```bash
psql -d postgres -c "SELECT pg_enhanced_query_logging_reset();" > /dev/null
psql -d postgres > /dev/null <<'SQL'
SET peql.rate_limit_auto_max_queries = 500;
SELECT 'auto_max_queries_test';
SQL

grep "Log_slow_rate_auto_max_queries" $PGDATA/log/peql-slow.log
```

**Expected:** a metadata line containing
`# Log_slow_rate_auto_max_queries: 500  Log_slow_rate_auto_max_bytes: 0`.
The `auto_max_bytes` value is 0 because it was not set.

### 11.8 Auto rate limit metadata (max bytes)

```bash
psql -d postgres -c "SELECT pg_enhanced_query_logging_reset();" > /dev/null
psql -d postgres > /dev/null <<'SQL'
SET peql.rate_limit_auto_max_bytes = 1048576;
SELECT 'auto_max_bytes_test';
SQL

grep "Log_slow_rate_auto_max_bytes" $PGDATA/log/peql-slow.log
```

**Expected:** a metadata line containing
`# Log_slow_rate_auto_max_queries: 0  Log_slow_rate_auto_max_bytes: 1048576`.
The `auto_max_queries` value is 0 because it was not set.

### 11.9 Auto rate limit metadata (both max queries and max bytes)

```bash
psql -d postgres -c "SELECT pg_enhanced_query_logging_reset();" > /dev/null
psql -d postgres > /dev/null <<'SQL'
SET peql.rate_limit_auto_max_queries = 200;
SET peql.rate_limit_auto_max_bytes = 524288;
SELECT 'auto_both_test';
SQL

grep "Log_slow_rate_auto" $PGDATA/log/peql-slow.log
```

**Expected:** a metadata line containing
`# Log_slow_rate_auto_max_queries: 200  Log_slow_rate_auto_max_bytes: 524288`.

### 11.10 No auto rate limit metadata when both are 0

```bash
psql -d postgres -c "SELECT pg_enhanced_query_logging_reset();" > /dev/null
psql -d postgres > /dev/null <<'SQL'
SET peql.rate_limit_auto_max_queries = 0;
SET peql.rate_limit_auto_max_bytes = 0;
SELECT 'no_auto_meta';
SQL

grep "Log_slow_rate_auto" $PGDATA/log/peql-slow.log
```

**Expected:** no output from grep. The auto rate limit metadata line is
omitted when both values are 0 (disabled).

### 11.11 All rate limit GUCs in metadata simultaneously

```bash
psql -d postgres -c "SELECT pg_enhanced_query_logging_reset();" > /dev/null
psql -d postgres > /dev/null <<'SQL'
SET peql.rate_limit = 3;
SET peql.rate_limit_type = 'session';
SET peql.rate_limit_always_log_duration = 5000;
SET peql.rate_limit_auto_max_queries = 100;
SET peql.rate_limit_auto_max_bytes = 262144;
SELECT 'all_rate_gucs';
SQL

grep -E "Log_slow_rate" $PGDATA/log/peql-slow.log
```

**Expected:** two metadata lines appear together:

```
# Log_slow_rate_type: session  Log_slow_rate_limit: 3  Log_slow_rate_limit_always_log_duration: 5000
# Log_slow_rate_auto_max_queries: 100  Log_slow_rate_auto_max_bytes: 262144
```

This confirms all five rate-limit GUC values are emitted in the output.

### 11.12 Session-mode rate limiting

```bash
psql -d postgres -c "SELECT pg_enhanced_query_logging_reset();" > /dev/null
psql -d postgres > /dev/null <<'SQL'
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

### 11.13 Rate limit metadata on utility statements

```bash
psql -d postgres -c "SELECT pg_enhanced_query_logging_reset();" > /dev/null
psql -d postgres > /dev/null <<'SQL'
SET peql.log_utility = on;
SET peql.rate_limit = 4;
SET peql.rate_limit_type = 'query';
SET peql.rate_limit_auto_max_queries = 300;
CREATE TABLE rate_util_test (id int);
DROP TABLE rate_util_test;
SQL

grep -E "Log_slow_rate" $PGDATA/log/peql-slow.log
```

**Expected:** both the rate-limit sampling metadata
(`Log_slow_rate_type: query  Log_slow_rate_limit: 4  Log_slow_rate_limit_always_log_duration: ...`)
and the auto rate-limit metadata (`Log_slow_rate_auto_max_queries: 300`)
appear on DDL entries, not just on regular queries.

## 12. Test Utility Statement Logging

### 12.1 DDL statements with log_utility = on

```bash
psql -d postgres -c "SELECT pg_enhanced_query_logging_reset();" > /dev/null
psql -d postgres > /dev/null <<'SQL'
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
psql -d postgres -c "SELECT pg_enhanced_query_logging_reset();" > /dev/null
psql -d postgres > /dev/null <<'SQL'
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
psql -d postgres > /dev/null <<'SQL'
CREATE OR REPLACE FUNCTION nested_test() RETURNS void AS $$
BEGIN
  PERFORM 1;
  PERFORM 2;
  PERFORM 3;
END;
$$ LANGUAGE plpgsql;
SQL

psql -d postgres -c "SELECT pg_enhanced_query_logging_reset();" > /dev/null
psql -d postgres > /dev/null <<'SQL'
SET peql.log_nested = off;
SELECT nested_test();
SQL

grep "PERFORM\|SELECT 1\|SELECT 2\|SELECT 3" $PGDATA/log/peql-slow.log
```

**Expected:** only the outer `SELECT nested_test()` call is logged.
The inner `PERFORM` statements do NOT appear.

### 13.2 PL/pgSQL function with log_nested = on

```bash
psql -d postgres -c "SELECT pg_enhanced_query_logging_reset();" > /dev/null
psql -d postgres > /dev/null <<'SQL'
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
psql -d postgres -c "SELECT pg_enhanced_query_logging_reset();" > /dev/null
psql -d postgres > /dev/null <<'SQL'
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
psql -d postgres -c "SELECT pg_enhanced_query_logging_reset();" > /dev/null
psql -d postgres > /dev/null <<'SQL'
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
psql -d postgres -c "SELECT pg_enhanced_query_logging_reset();" > /dev/null
psql -d postgres > /dev/null <<'SQL'
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
psql -d postgres -c "SELECT pg_enhanced_query_logging_reset();" > /dev/null
psql -d postgres > /dev/null <<'SQL'
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
psql -d postgres -c "SELECT pg_enhanced_query_logging_reset();" > /dev/null
psql -d postgres > /dev/null <<'SQL'
SET peql.log_query_plan = on;
SET peql.log_query_plan_format = 'json';
SELECT count(*) FROM big_tbl WHERE id > 50000;
SQL

grep -A 10 "# Plan:" $PGDATA/log/peql-slow.log
```

**Expected:** EXPLAIN output in JSON format after the `# Plan:` line.

### 15.3 Plan not logged when log_query_plan = off

```bash
psql -d postgres -c "SELECT pg_enhanced_query_logging_reset();" > /dev/null
psql -d postgres > /dev/null <<'SQL'
SET peql.log_query_plan = off;
SELECT count(*) FROM big_tbl WHERE id > 50000;
SQL

grep "Plan:" $PGDATA/log/peql-slow.log
```

**Expected:** no output (plan is not included).

## 16. Test Enable/Disable Toggle

### 16.1 peql.enabled = off disables all logging

```bash
psql -d postgres -c "SELECT pg_enhanced_query_logging_reset();" > /dev/null
psql -d postgres -c "SET peql.enabled = off; SELECT 'should_not_appear';" > /dev/null
grep "should_not_appear" $PGDATA/log/peql-slow.log
```

**Expected:** no output.

### 16.2 peql.log_min_duration = -1 disables logging

```bash
psql -d postgres -c "SELECT pg_enhanced_query_logging_reset();" > /dev/null
psql -d postgres -c "SET peql.log_min_duration = '-1'; SELECT 'also_hidden';" > /dev/null
grep "also_hidden" $PGDATA/log/peql-slow.log
```

**Expected:** no output.

### 16.3 peql.log_min_duration threshold filtering

```bash
psql -d postgres -c "SELECT pg_enhanced_query_logging_reset();" > /dev/null
psql -d postgres > /dev/null <<'SQL'
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
psql -d postgres -c "SELECT pg_enhanced_query_logging_reset();" > /dev/null
psql -d postgres > /dev/null <<'SQL'
SET peql.log_verbosity = 'full';
SET peql.track_planning = on;
SELECT count(*) FROM big_tbl WHERE id > 50000;
SQL

grep "Plan_time" $PGDATA/log/peql-slow.log
```

**Expected:** `# Plan_time:` line with a nonzero value.

```bash
psql -d postgres -c "SELECT pg_enhanced_query_logging_reset();" > /dev/null
psql -d postgres > /dev/null <<'SQL'
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
| Rate limit metadata in output          | `Log_slow_rate_type` / `Log_slow_rate_limit` / `Log_slow_rate_limit_always_log_duration` present when sampling |
| No rate metadata when rate_limit=1     | `Log_slow_rate_type` absent when not sampling                         |
| Session-mode rate limiting             | All-or-nothing logging per session                                    |
| Always-log-duration override           | Very slow queries logged despite high rate_limit                      |
| Always-log-duration value in metadata  | `Log_slow_rate_limit_always_log_duration` reflects the configured value |
| Auto max queries metadata              | `Log_slow_rate_auto_max_queries` present when > 0                     |
| Auto max bytes metadata                | `Log_slow_rate_auto_max_bytes` present when > 0                       |
| No auto metadata when both are 0       | `Log_slow_rate_auto_*` lines absent when both values are 0            |
| All rate GUCs in metadata together     | Both rate-limit metadata lines appear with all five GUC values        |
| Rate metadata on utility statements    | Rate-limit metadata appears on DDL entries, not just regular queries  |
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
psql -d postgres -c "SET work_mem = '64kB'; SELECT * FROM generate_series(1, 100000) AS n ORDER BY n DESC;" > /dev/null

# Force temp-table on disk (Temp_table_on_disk: Yes)
psql -d postgres -c "SET work_mem = '64kB'; WITH big AS (SELECT generate_series(1, 100000) AS n) SELECT count(*) FROM big;" > /dev/null
```

## 20. Automated Tests

In addition to the manual tests above, the extension includes automated test
suites. A shared helper module (`t/PeqlNode.pm`) provides common setup,
teardown, and log-reading logic used by the TAP tests.

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

Or run `prove` directly (required when PostgreSQL is built from source):

```bash
export PG_REGRESS=/opt/postgresql/18.3/lib/pgxs/src/test/regress/pg_regress
prove -v -I /Users/agustin/src/postgres/src/test/perl t/*.pl
```

See the [Prerequisites](#prerequisites) section for required environment setup.

Test files:
- `t/001_basic_logging.pl` -- log file creation, pt-query-digest format, reset function, enable/disable
- `t/002_rate_limiting.pl` -- rate limit sampling, metadata output, always-log override
- `t/003_extended_metrics.pl` -- verbosity levels, buffer/WAL metrics, plan quality indicators, utility logging, row counts
- `t/004_nested_logging.pl` -- nested statement logging (PL/pgSQL inner statements)
- `t/005_parameter_values.pl` -- parameter value logging, NULL handling, prepared statements
- `t/006_query_plan.pl` -- EXPLAIN plan output in text and JSON format
- `t/007_planning_time.pl` -- planning time tracking with verbosity levels
- `t/008_min_duration.pl` -- duration threshold filtering with pg_sleep
- `t/009_session_rate_limit.pl` -- session-mode rate limiting (all-or-nothing)
- `t/010_edge_cases.pl` -- disk sort, disk temp table, memory tracking, schema field changes

### 20.3 Meson Build

The extension supports building with Meson (PostgreSQL 16+) in addition to
Make. When building as part of the PostgreSQL source tree:

```bash
meson setup build
cd build
meson test -C . --suite pg_enhanced_query_logging
```

The `meson.build` file declares the shared module, regression tests, TAP
tests, and extension data files.

### 20.4 CI/CD (Continuous Integration / Continuous Delivery)

Automated tests run on every push and pull request via GitHub Actions
(`.github/workflows/test.yml`). The CI matrix covers:

- **Platforms**: Ubuntu and macOS
- **PostgreSQL versions**: 17 and 18
- **Test suites**: pg_regress (SQL) and TAP (Perl)

Test artifacts (diffs, logs) are uploaded on failure for debugging.

## 21. Clean Up

```bash
pg_ctl -D $PGDATA stop
rm -rf /tmp/peql_test
```
