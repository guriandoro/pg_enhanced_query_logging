# Disk Space Protection

[Back to README](../README.md)

## Overview

peql writes slow-query log entries to a dedicated file on every qualifying query. On a busy system this file can grow quickly, and if the underlying filesystem fills up, the consequences extend beyond peql -- PostgreSQL itself may fail to write WAL, checkpoints, or temporary files.

To stay unintrusive, peql monitors free space on the mountpoint that hosts the log directory and **automatically pauses logging** when it drops below a configurable threshold. Logging resumes transparently once space recovers. An optional auto-purge mechanism can delete old rotated log files to reclaim space without DBA intervention.

## Design Principles

1. **Near-zero overhead in normal operation.** The disk check adds two atomic reads (~2 ns) per query on the fast path. The actual `statvfs()` syscall runs at most once per check interval, by exactly one backend.
2. **No background worker.** The check piggybacks on regular query execution. If no queries are running, no checks happen -- but no logs are being written either, so there is no risk.
3. **No GUC modification.** The extension never changes `peql.enabled` or writes to `postgresql.conf`. The shared-memory pause flag achieves the same effect (all backends stop logging) without side effects that could surprise a DBA.
4. **Opt-in auto-purge.** Automatic file deletion is disabled by default. It must be explicitly enabled by the DBA.

## Configuration

| GUC | Type | Default | Context | Description |
|-----|------|---------|---------|-------------|
| `peql.disk_threshold_pct` | int (0-100) | `5` | SUSET | Pause logging when free space drops below this percentage. `0` disables the check entirely. |
| `peql.disk_check_interval_ms` | int (ms) | `5000` | SUSET | Minimum interval between `statvfs()` calls. Lower values detect low-disk faster but add syscall overhead. Minimum: 100 ms. |
| `peql.disk_auto_purge` | bool | `off` | SUSET | When enabled and disk space is below the threshold, automatically delete old rotated log files (`.old` suffix) created by `pg_enhanced_query_logging_reset()`. |

### Examples

```sql
-- Enable disk protection with a 10% threshold, checking every 3 seconds
SET peql.disk_threshold_pct = 10;
SET peql.disk_check_interval_ms = 3000;

-- Also enable automatic purging of old rotated logs
SET peql.disk_auto_purge = on;

-- Disable disk protection entirely
SET peql.disk_threshold_pct = 0;
```

## How It Works

### Layer 1: Threshold Check (Primary Mechanism)

Every time a query passes the duration and rate-limit filters, `peql_should_log()` calls `peql_disk_space_ok()` before formatting or writing the log entry:

1. **Fast path:** Read the shared-memory `disk_paused` flag (one atomic read). If the check interval has not elapsed, return the cached result immediately.
2. **Check path:** If the interval has elapsed, attempt a compare-and-exchange (CAS) on `last_disk_check_usec`. Only the backend that wins the CAS calls `statvfs()` on the log directory's mountpoint. All other backends fall through to the cached flag.
3. **Pause/resume:** If `f_bavail / f_blocks * 100 < peql.disk_threshold_pct`, the backend sets `disk_paused = 1` and emits a single `LOG`-level message. When a subsequent check finds space has recovered, it clears the flag and emits a "resumed" message.

The CAS pattern mirrors the existing `peql_adaptive_maybe_reset_window()` used by the adaptive rate limiter, ensuring exactly one `statvfs()` call per interval regardless of connection count.

### Layer 2: Auto-Purge (Optional)

When `peql.disk_auto_purge` is enabled and the threshold check detects low disk:

1. The checking backend attempts to acquire the `purge_in_progress` atomic flag (test-and-set).
2. If acquired, it scans the log directory for files matching `<peql.log_filename>.old*` -- these are the files created by `pg_enhanced_query_logging_reset()`.
3. Each matching file is deleted with `unlink()`, and a `LOG`-level message is emitted per file.
4. The flag is released so future intervals can purge again if needed.

The auto-purge **never deletes the active log file**. It only targets files with the `.old` suffix that the extension itself created during rotation.

## Monitoring

The `pg_enhanced_query_logging_stats()` function exposes the disk protection state:

```sql
SELECT * FROM pg_enhanced_query_logging_stats();
 queries_logged | queries_skipped | bytes_written | disk_paused | disk_skipped
----------------+-----------------+---------------+-------------+--------------
            142 |               8 |         48320 | f           |            0
(1 row)
```

| Column | Type | Description |
|--------|------|-------------|
| `disk_paused` | boolean | `true` if logging is currently paused due to low disk space |
| `disk_skipped` | bigint | Total queries skipped due to low disk since last reset or server start |

When `disk_paused` is `true`, all backends skip log writes. This is the signal to investigate disk usage. The `disk_skipped` counter shows how many queries were affected.

The `disk_skipped` counter resets to zero when `pg_enhanced_query_logging_reset()` is called or the server restarts.

## Concurrency and Scalability

The implementation is designed for high-concurrency workloads (thousands of connections):

| Operation | Per-query cost | Behavior at scale |
|-----------|---------------|-------------------|
| Read `disk_paused` flag | ~1 ns (1 atomic read) | No contention; each backend reads independently |
| Read `last_disk_check_usec` | ~1 ns (1 atomic read) | No contention |
| CAS on `last_disk_check_usec` | 1 CAS when interval expires | Exactly 1 winner per interval |
| `statvfs()` syscall | ~1-5 us (kernel VFS cache) | Exactly 1 call per interval, regardless of connection count |
| Increment `total_disk_skipped` | ~5-20 ns (1 atomic add) | Only during disk-low state; same pattern as `total_queries_skipped` |

**Fast path (normal operation):** 2 atomic reads per query. At 100,000 queries/second across 1,000 backends, this adds approximately 0.2 ms of total CPU time per second across all backends combined.

**Thundering herd prevention:** The compare-and-exchange on `last_disk_check_usec` ensures that when the check interval expires, only one backend performs the `statvfs()` syscall. All others see the CAS fail and immediately read the cached `disk_paused` flag. This is the same lock-free pattern used by the adaptive rate limiter's window reset.

**Auto-purge serialization:** The `purge_in_progress` atomic flag (test-and-set) ensures only one backend scans the directory and deletes files at a time. If a second backend enters the check while a purge is running, it skips the purge and reads the cached flag.

**Stale reads are benign:** Between checks, backends may read a slightly stale `disk_paused` value. The staleness window is bounded by `peql.disk_check_interval_ms` (default 5 seconds). This is an acceptable trade-off: if disk fills between checks, a few queries may still attempt writes (which will fail with a short-write warning) before the next check pauses logging.

## Behavior Details

- **One-time LOG messages:** The extension emits a `LOG`-level message when transitioning to paused state and again when resuming. It does not emit a message per skipped query, to avoid log spam.
- **Interaction with rate limiting:** The disk check runs before the rate limiter. If disk is low, the query is skipped immediately without consuming a rate-limit slot.
- **Interaction with `peql.enabled`:** If `peql.enabled = off`, no hooks fire and the disk check is never reached. The disk check only matters when the extension is actively logging.
- **`statvfs()` failure:** If the syscall fails (e.g., the directory was removed), the extension logs a warning and allows the write to proceed. It does not pause logging on a failed check.
- **Empty filesystem (`f_blocks = 0`):** Treated as "space is fine" to avoid division by zero on pseudo-filesystems.

## Platform Notes

- **Linux, macOS, FreeBSD:** Full support via POSIX `statvfs()`.
- **Windows:** `statvfs()` is not available. The disk check is a no-op (always returns "ok"). A future release may add support via `GetDiskFreeSpaceEx()`.

## Why Not Disable `peql.enabled` Automatically?

`peql.enabled` is a `SUSET` GUC -- it can only be changed by superusers via `SET` or `ALTER SYSTEM`. An extension hook cannot call `SetConfigOption("peql.enabled", "off", ...)` and have it take effect globally; it would only affect the current session.

The alternative -- a background worker that runs `ALTER SYSTEM SET peql.enabled = off` and `pg_reload_conf()` -- is technically possible but was rejected as too heavy-handed. It would modify `postgresql.auto.conf`, require the DBA to manually re-enable the extension after resolving the disk issue, and could be surprising in automated environments.

The shared-memory `disk_paused` flag achieves the same practical effect (all backends stop logging) without any of these drawbacks. It is transparent, automatic, and self-healing.
