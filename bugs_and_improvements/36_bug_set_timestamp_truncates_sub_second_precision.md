# Bug: `SET timestamp` start-time computation truncates sub-second duration

## Status: Open

## Priority: Medium

## Effort: Trivial

## Description

The `SET timestamp` line is supposed to reflect the query's start time,
computed by subtracting the duration from the completion time. The current
code uses a truncating integer cast:

```c
appendStringInfo(buf, "SET timestamp=" INT64_FORMAT ";\n",
                 (int64) stamp_time - (int64)(duration_ms / 1000.0));
```

The cast `(int64)(duration_ms / 1000.0)` truncates toward zero. Examples:

| `duration_ms` | `duration_ms / 1000.0` | `(int64)` cast | Error |
|---------------|------------------------|----------------|-------|
| 999           | 0.999                  | 0              | ~1s   |
| 1999          | 1.999                  | 1              | ~1s   |
| 500           | 0.500                  | 0              | ~0.5s |

For a query that took 999ms, the subtracted value is 0, making the reported
start time identical to the completion time. The start time is systematically
reported up to 1 second later than reality.

This is a follow-up precision issue to the fix in bug #26, which correctly
changed `SET timestamp` to subtract the duration but used a truncating cast.

The same issue exists in both `peql_format_entry` (line 1297) and
`peql_format_utility_entry` (line 1549).

## Location

`pg_enhanced_query_logging.c`, lines 1296-1297 and 1548-1549

## Fix

Use rounding instead of truncation:

```c
appendStringInfo(buf, "SET timestamp=" INT64_FORMAT ";\n",
                 (int64) stamp_time - (int64) round(duration_ms / 1000.0));
```

Or use `lround()`:

```c
appendStringInfo(buf, "SET timestamp=" INT64_FORMAT ";\n",
                 (int64) stamp_time - lround(duration_ms / 1000.0));
```

Apply the same fix in both `peql_format_entry` and `peql_format_utility_entry`.
