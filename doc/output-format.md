# Output Format

[Back to README](../README.md)

The log format is designed to be parsed by `pt-query-digest --type slowlog`. Every entry follows the MySQL slow query log structure with `# Key: Value` header lines.

## Minimal Verbosity

The core fields required by pt-query-digest, emitted at all verbosity levels:

```
# Time: 2026-02-27T14:30:00.123456
# User@Host: alice[alice] @ 192.168.1.10 []
# Query_time: 0.523411  Lock_time: 0.000000  Rows_sent: 42  Rows_examined: 15000
SET timestamp=1772147400;
SELECT * FROM orders WHERE status = 'pending';
```

## Standard Verbosity

Adds connection metadata:

```
# Time: 2026-02-27T14:30:00.123456
# User@Host: alice[alice] @ 192.168.1.10 []
# Thread_id: 12345  Schema: mydb.public
# Bytes_sent: 4096
# Query_id: -6432758210044805760
# Query_time: 0.523411  Lock_time: 0.000000  Rows_sent: 42  Rows_examined: 15000
# Rows_affected: 0
SET timestamp=1772147400;
SELECT * FROM orders WHERE status = 'pending';
```

## Full Verbosity

Adds buffer, WAL, JIT, plan quality, and memory metrics:

```
# Time: 2026-02-27T14:30:00.123456
# User@Host: alice[alice] @ 192.168.1.10 []
# Thread_id: 12345  Schema: mydb.public
# Bytes_sent: 4096
# Query_id: -6432758210044805760
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

## Field Reference

| Field | Source | Verbosity | Description |
|-------|--------|-----------|-------------|
| `Time` | `GetCurrentTimestamp()` | all | ISO-8601 timestamp with microsecond precision |
| `User@Host` | `MyProcPort` | all | Connecting user and remote host |
| `Thread_id` | `MyProcPid` | standard+ | PostgreSQL backend PID (Process ID) |
| `Schema` | `fetch_search_path()` | standard+ | Database and schema in `db.schema` format |
| `Bytes_sent` | `rows * plan_width` | standard+ | Estimated bytes sent to the client (planner row width times rows processed) |
| `Query_id` | `Query->queryId` | standard+ | Query identifier for cross-referencing with `pg_stat_statements` (requires `compute_query_id = on` or `auto`) |
| `Query_time` | `Instrumentation.total` | all | Total execution time in seconds |
| `Lock_time` | -- | all | Reserved (always 0; PostgreSQL doesn't expose per-query lock wait time the same way MySQL does) |
| `Rows_sent` | `es_processed` (SELECT) | all | Rows returned to the client |
| `Rows_examined` | Plan tree ntuples sum | all | Rows scanned across all plan nodes |
| `Rows_affected` | `es_processed` (DML, Data Manipulation Language) | standard+ | Rows modified by INSERT/UPDATE/DELETE |
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
| `Table_io` | Per-node `BufferUsage` | full | Per-table I/O attribution showing buffer hits and reads for the top 5 tables |
| `Mem_allocated` | `MemoryContextMemAllocated()` | full | Bytes allocated in the query memory context (requires `peql.track_memory`) |
| `Wait_events` | Periodic sampling | full | Histogram of wait events observed during execution (requires `peql.track_wait_events`) |
| `JIT_functions` | `JitInstrumentation` | full | Number of JIT-compiled functions |
| `JIT_generation_time` | `JitInstrumentation` | full | Time spent generating JIT code (seconds) |
| `JIT_inlining_time` | `JitInstrumentation` | full | Time spent inlining JIT code (seconds) |
| `JIT_optimization_time` | `JitInstrumentation` | full | Time spent optimizing JIT code (seconds) |
| `JIT_emission_time` | `JitInstrumentation` | full | Time spent emitting JIT code (seconds) |
| `Log_slow_rate_type` | GUC | all (when sampling) | Rate limit mode (`session` or `query`) |
| `Log_slow_rate_limit` | GUC | all (when sampling) | Configured rate limit value |
| `Log_slow_rate_limit_always_log_duration` | GUC | all (when sampling) | Duration threshold (ms) that bypasses the rate limiter (`-1` = disabled) |
| `Log_slow_rate_auto_max_queries` | GUC | all (when adaptive) | Configured max queries/second cluster-wide |
| `Log_slow_rate_auto_max_bytes` | GUC | all (when adaptive) | Configured max bytes/second cluster-wide |

## Key Naming Conventions

The field names are chosen to work with pt-query-digest's `# (\w+): (\S+)` parser:

- Fields ending in `_time` are parsed as float/time metrics
- `Yes`/`No` values are parsed as booleans
- Everything else is parsed as integers
