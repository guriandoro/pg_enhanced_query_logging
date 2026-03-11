# Architecture

[Back to README](../README.md)

## Hook Chain

The extension installs hooks in `_PG_init()`, chaining with any previously installed hooks so it coexists with other extensions (e.g., `pg_stat_statements`, `auto_explain`):

```
                                        ┌──────────────────────────┐
                                        │   shared_preload_libs    │
                                        │  loads _PG_init()        │
                                        └────────────┬─────────────┘
                                                     │
     ┌───────────────────────────────────────────────┼───────────────────────────────────────┐
     │                    │                          │                                       │
┌────▼─────────────┐ ┌───▼──────────┐   ┌───────────▼────────────┐              ┌───────────▼───────────┐
│ post_parse_       │ │ planner_hook │   │  ExecutorStart_hook    │              │  ProcessUtility_hook  │
│ analyze_hook      │ │ measure plan │   │  enable INSTRUMENT_ALL │              │  time DDL statements  │
│ capture query_id  │ │ time         │   └────────────┬───────────┘              └───────────────────────┘
└──────────────────┘ └──────────────┘                 │
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

## Rate Limiting

The extension implements the Percona Server rate limiting model:

- **Session mode** (`peql.rate_limit_type = 'session'`): On first query in a backend, draw once from the PRNG (Pseudo-Random Number Generator). If selected (1-in-N chance), every query in this session is logged. This produces complete session traces for sampled sessions.
- **Query mode** (`peql.rate_limit_type = 'query'`): Each query independently draws from the PRNG with a 1-in-N chance of being logged. This gives a uniform sample across all sessions.
- **Always-log override**: Queries exceeding `peql.rate_limit_always_log_duration` bypass the rate limiter entirely, ensuring very slow queries are never missed.

Rate limiting uses PostgreSQL's built-in PRNG (`pg_prng_uint64()` from `pg_global_prng_state`). For a configured rate limit of *N*, the extension draws a random 64-bit integer and computes `r = random % N`; the query is logged only if `r == 0`, giving a uniform 1-in-N probability. In session mode, this draw happens once per backend (on the first query) and the result is cached for the lifetime of the session. In query mode, each query draws independently.

## File I/O

- Uses raw POSIX `open(O_WRONLY | O_APPEND | O_CREAT)` / `write()` / `close()` for each log entry
- Each backend opens, appends, and closes the file per log entry -- no persistent file handle
- `O_APPEND` guarantees atomic writes on POSIX for entries under `PIPE_BUF` (typically 4-64 KB)
- If the log directory doesn't exist, the extension creates it automatically via `MakePGDirectory()`
- Log path resolution: `peql.log_directory` overrides PostgreSQL's `log_directory`; relative paths resolve against `DataDir`

## Disk Space Protection

Before formatting or writing a log entry, `peql_should_log()` calls `peql_disk_space_ok()` to check free space on the log mountpoint via POSIX `statvfs()`. The check uses a compare-and-exchange (CAS) on a shared-memory timestamp so that exactly one backend performs the syscall per `peql.disk_check_interval_ms`, regardless of connection count. All other backends read the cached `disk_paused` flag (a single atomic read, ~1 ns).

When free space drops below `peql.disk_threshold_pct`, the flag is set and all backends skip logging. When space recovers, the flag is cleared. Optional auto-purge (`peql.disk_auto_purge`) deletes old rotated log files to reclaim space.

See [Disk Space Protection](disk-space-protection.md) for the full design, concurrency analysis, and configuration reference.

## Plan Tree Analysis

At `full` verbosity, the extension walks the executed plan tree using `planstate_tree_walker` to extract:

- **Full_scan**: Any `SeqScan` node present
- **Filesort**: Any `Sort` node present
- **Filesort_on_disk**: Sort node that spilled to disk (via `TuplesortInstrumentation`)
- **Temp_table**: Any `Material` node present
- **Temp_table_on_disk**: Temp blocks written > 0
- **Rows_examined**: Sum of `ntuples` across all scan nodes (more accurate than `Rows_sent`)
