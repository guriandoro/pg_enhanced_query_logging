# Testing pg_enhanced_query_logging

Manual testing guide for the pg_enhanced_query_logging extension against
PostgreSQL 18.3 installed at `/opt/postgresql/18.3`.

## Prerequisites

- PostgreSQL 18.3 installed at `/opt/postgresql/18.3`
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
peql.track_planning        = on         # show Plan_time
peql.track_wal             = on         # show WAL_records / WAL_bytes / WAL_fpi
peql.track_io_timing       = on         # show Shared_blk_read_time / write_time
peql.track_memory          = on         # show Mem_allocated

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

## 6. Run Test Queries

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

## 11. Verification Checklist

| Check                                  | How to verify                                                         |
| -------------------------------------- | --------------------------------------------------------------------- |
| Extension loads without errors         | No errors in `$PGDATA/logfile` on startup                             |
| Queries are logged                     | Entries appear in `$PGDATA/log/peql-slow.log`                         |
| `Schema` shows `db.schema`            | e.g. `postgres.public` not just `postgres`                            |
| `Rows_examined` uses plan-tree data    | Value may differ from `Rows_sent` on filtered queries                 |
| Buffer counters are populated          | Nonzero `Shared_blks_*` for table scans                               |
| WAL counters are populated             | Nonzero `WAL_records` on INSERT/UPDATE/DELETE                         |
| `Plan_time` appears                    | Present when `peql.track_planning = on`                               |
| `Full_scan: Yes` on SeqScan           | Any `SELECT` without an index                                         |
| `Filesort: Yes` on ORDER BY           | Queries with `ORDER BY`                                               |
| `Filesort_on_disk: Yes`               | Large sorts exceeding `work_mem` (set `work_mem = '64kB'` to force)   |
| `Temp_table: Yes` on Material nodes   | Queries with CTEs or subqueries that materialize                      |
| JIT metrics appear                     | Lower `jit_above_cost` and run an expensive aggregate                 |
| `Mem_allocated` appears                | When `peql.track_memory = on`                                         |
| Verbosity levels work                  | `minimal` < `standard` < `full` produce progressively more fields     |
| `peql.log_min_duration = -1` disables  | No new entries appear                                                 |
| Reset function truncates the file      | `pg_enhanced_query_logging_reset()` empties the file                  |
| pt-query-digest can parse the output   | `pt-query-digest --type slowlog` produces a report without errors     |

## 12. Forcing Edge Cases

```bash
# Force disk-based sort (Filesort_on_disk: Yes)
psql -d postgres -c "SET work_mem = '64kB'; SELECT * FROM generate_series(1, 100000) AS n ORDER BY n DESC;"

# Force temp-table on disk (Temp_table_on_disk: Yes)
psql -d postgres -c "SET work_mem = '64kB'; WITH big AS (SELECT generate_series(1, 100000) AS n) SELECT count(*) FROM big;"
```

## 13. Clean Up

```bash
pg_ctl -D $PGDATA stop
rm -rf /tmp/peql_test
```
