# Configuration Reference

[Back to README](../README.md)

All GUC (Grand Unified Configuration) variables are prefixed with `peql.` and can be set in `postgresql.conf` or at runtime (within their context restrictions).

## Core Settings

| GUC | Type | Default | Context | Description |
|-----|------|---------|---------|-------------|
| `peql.enabled` | bool | `on` | SUSET | Master on/off switch for the extension |
| `peql.log_min_duration` | int (ms) | `-1` | SUSET | Minimum execution time to log a query. `-1` disables logging entirely; `0` logs all queries |
| `peql.log_directory` | string | `""` | SIGHUP | Directory for the log file. Empty string uses PostgreSQL's `log_directory` |
| `peql.log_filename` | string | `"peql-slow.log"` | SIGHUP | Name of the slow query log file |
| `peql.log_verbosity` | enum | `standard` | SUSET | Detail level: `minimal`, `standard`, or `full` |

## Statement Filtering

| GUC | Type | Default | Context | Description |
|-----|------|---------|---------|-------------|
| `peql.log_utility` | bool | `off` | SUSET | Log utility (DDL) statements via the ProcessUtility hook |
| `peql.log_nested` | bool | `off` | SUSET | Log statements nested inside PL/pgSQL functions and procedures |
| `peql.log_transaction` | bool | `off` | SUSET | Log at transaction boundary, aggregating metrics across all statements |

## Rate Limiting

| GUC | Type | Default | Context | Description |
|-----|------|---------|---------|-------------|
| `peql.rate_limit` | int | `1` | SUSET | Log every Nth query or session. `1` = log all eligible queries |
| `peql.rate_limit_type` | enum | `query` | SUSET | Sampling mode: `session` (decide once per backend) or `query` (decide per statement) |
| `peql.rate_limit_always_log_duration` | int (ms) | `10000` | SUSET | Queries slower than this bypass the rate limiter entirely. `0` = bypass for all queries, `-1` = disabled |
| `peql.rate_limit_auto_max_queries` | int | `0` | SUSET | Max queries/second to log cluster-wide (shared memory). `0` = disabled |
| `peql.rate_limit_auto_max_bytes` | int | `0` | SUSET | Max bytes/second to log cluster-wide (shared memory). `0` = disabled |

## Disk Space Protection

| GUC | Type | Default | Context | Description |
|-----|------|---------|---------|-------------|
| `peql.disk_threshold_pct` | int (0-100) | `5` | SUSET | Pause logging when free disk space drops below this %. `0` = disabled |
| `peql.disk_check_interval_ms` | int (ms) | `5000` | SUSET | Minimum interval between disk space checks. Lower values detect low-disk faster. Min: 100 ms |
| `peql.disk_auto_purge` | bool | `off` | SUSET | Automatically delete old rotated (`.old`) log files when disk is low |

See [Disk Space Protection](disk-space-protection.md) for full details on behavior, concurrency, and monitoring.

## Extended Tracking

| GUC | Type | Default | Context | Description |
|-----|------|---------|---------|-------------|
| `peql.track_io_timing` | bool | `on` | SUSET | Include block read/write times (requires PostgreSQL's `track_io_timing = on`) |
| `peql.track_wal` | bool | `on` | SUSET | Include WAL usage metrics (records, bytes, full-page images) |
| `peql.track_memory` | bool | `off` | SUSET | Include memory context allocation (experimental, adds overhead) |
| `peql.track_planning` | bool | `off` | SUSET | Track and log planning time separately from execution time |
| `peql.track_wait_events` | bool | `off` | SUSET | Sample and log wait events during execution (experimental, 10ms timer) |

## Query Plan and Parameters

| GUC | Type | Default | Context | Description |
|-----|------|---------|---------|-------------|
| `peql.log_parameter_values` | bool | `off` | SUSET | Append bind parameter values to the log entry |
| `peql.log_query_plan` | bool | `off` | SUSET | Include EXPLAIN ANALYZE output in the log entry |
| `peql.log_query_plan_format` | enum | `text` | SUSET | EXPLAIN output format: `text` or `json` |

## Context Values

- **SUSET**: Can be changed by superusers at runtime with `SET` or `ALTER SYSTEM`
- **SIGHUP**: Requires a configuration reload (`pg_ctl reload` or `SELECT pg_reload_conf()`)

## Runtime Examples

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
