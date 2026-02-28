# Bug: GUC_UNIT_MS on `peql.log_min_duration` causes threshold comparison mismatch

## Status: Not fixed

## Priority: High

## Effort: Trivial

## Description

The GUC `peql.log_min_duration` is defined with `GUC_UNIT_MS`:

```c
DefineCustomIntVariable("peql.log_min_duration",
                        ...,
                        &peql_log_min_duration,
                        -1,
                        -1, INT_MAX,
                        PGC_SUSET,
                        GUC_UNIT_MS,
                        NULL, NULL, NULL);
```

When a GUC has `GUC_UNIT_MS`, PostgreSQL stores the value internally in
milliseconds but accepts human-readable units. For example, `SET
peql.log_min_duration = '5s'` stores `5000` in `peql_log_min_duration`.

However, `peql.rate_limit_always_log_duration` also has `GUC_UNIT_MS`:

```c
DefineCustomIntVariable("peql.rate_limit_always_log_duration",
                        ...,
                        &peql_rate_limit_always_log_duration,
                        10000,       /* 10 seconds */
                        0, INT_MAX,
                        PGC_SUSET,
                        GUC_UNIT_MS,
                        NULL, NULL, NULL);
```

Both GUCs store values in milliseconds internally. The duration comparison in
`peql_ExecutorEnd` and `peql_should_log` is:

```c
msec = queryDesc->totaltime->total * 1000.0;   /* seconds -> ms */

if (msec >= peql_log_min_duration ...)          /* ms vs ms -- OK */

if (duration_ms >= peql_rate_limit_always_log_duration) /* ms vs ms -- OK */
```

This is actually correct for the base case. **However**, when a user sets the
value with a different unit suffix, PostgreSQL converts it. For example:

```sql
SET peql.log_min_duration = '2s';
```

PostgreSQL stores `2000` (ms) in the variable, which is correct. But consider:

```sql
SET peql.log_min_duration = '2min';
```

PostgreSQL stores `120000` (ms). This is also correct.

**The actual bug is in `SHOW`**: When displayed, the GUC framework shows the
value in the most human-readable unit. `SHOW peql.log_min_duration` for a
value of `500` shows `500ms`. For `10000` it shows `10s`. This is cosmetic
but can confuse users who set it to `100` expecting milliseconds and see
`100ms` -- that's correct. This is not a bug, just a potential documentation
clarity issue.

**HOWEVER** -- re-examining more carefully, the comparison in `peql_should_log`
does have a real issue:

```c
if (peql_rate_limit_always_log_duration > 0 &&
    duration_ms >= peql_rate_limit_always_log_duration)
```

When `peql_rate_limit_always_log_duration` is set to `0` (i.e., "always bypass
the rate limiter"), the `> 0` check prevents it from firing. A value of `0`
should mean "all queries bypass the rate limiter" (since any query with
duration >= 0 should match). But the `> 0` guard skips the check entirely.

This means setting `peql.rate_limit_always_log_duration = 0` silently
**disables** the always-log override instead of enabling it for all queries.

## Location

`pg_enhanced_query_logging.c`, line 636 (`peql_should_log`)

## Fix

Change the guard to allow `0` as a valid threshold:

```c
if (peql_rate_limit_always_log_duration >= 0 &&
    duration_ms >= peql_rate_limit_always_log_duration)
    return true;
```

Or more explicitly, since `peql_rate_limit_always_log_duration` is always >= 0
(its minimum is `0`), simplify to:

```c
if (duration_ms >= peql_rate_limit_always_log_duration)
    return true;
```

Then document that `0` means "bypass rate limiter for all queries" and consider
using `-1` as the sentinel for "disable the always-log override."

**Note**: The TAP test `002_rate_limiting.pl` test 5 sets
`rate_limit_always_log_duration = 0` and expects it to bypass the rate limiter.
This test currently passes because with `duration >= 0` and `0 > 0` being false,
the query still gets logged via the normal path (the query's own rate limiter
check). The test is not actually validating the always-log-duration path at all.
