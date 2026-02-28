# Bug: `%ld` for SET timestamp truncates on 32-bit / Windows platforms

## Status: Not fixed

## Priority: Medium

## Effort: Trivial

## Description

The `SET timestamp=` line is formatted with:

```c
appendStringInfo(buf, "SET timestamp=%ld;\n", (long) stamp_time);
```

`pg_time_t` is `int64` on some platforms, but `long` is only 32 bits on Windows
(including 64-bit Windows) and on 32-bit Unix systems. After 2038-01-19, the
cast to `(long)` will truncate the timestamp, producing incorrect values.

The same issue exists in `peql_format_utility_entry` at line 1362.

## Location

`pg_enhanced_query_logging.c`, lines 1138 and 1362

## Fix

Use `INT64_FORMAT` and cast to `int64`:

```c
appendStringInfo(buf, "SET timestamp=" INT64_FORMAT ";\n", (int64) stamp_time);
```
