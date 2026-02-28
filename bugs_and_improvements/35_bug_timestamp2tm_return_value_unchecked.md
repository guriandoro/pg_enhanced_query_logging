# Bug: `timestamp2tm` return value not checked in header formatter

## Status: Fixed

## Priority: Medium

## Effort: Easy

## Description

In `peql_format_header`, the return value of `timestamp2tm` is silently
discarded:

```c
timestamp2tm(now, &tz, &tm_result, &fsec, NULL, NULL);
usec = (int) fsec;

snprintf(timebuf, sizeof(timebuf),
         "%04d-%02d-%02dT%02d:%02d:%02d.%06d",
         tm_result.tm_year, tm_result.tm_mon, ...);
```

`timestamp2tm` returns 0 on success and non-zero on failure (e.g., timestamp
out of range). If it fails, the `tm_result` fields will contain
uninitialised/garbage values, producing a malformed `# Time:` line that could
confuse pt-query-digest or other log consumers.

In practice this is unlikely because `now` comes from `GetCurrentTimestamp()`
which always returns a valid value, but the missing check is a robustness gap.

## Location

`pg_enhanced_query_logging.c`, line 993 (`peql_format_header`)

## Fix

Check the return value and fall back to a placeholder on failure:

```c
if (timestamp2tm(now, &tz, &tm_result, &fsec, NULL, NULL) != 0)
{
    snprintf(timebuf, sizeof(timebuf), "0000-00-00T00:00:00.000000");
    usec = 0;
}
else
{
    usec = (int) fsec;
    snprintf(timebuf, sizeof(timebuf),
             "%04d-%02d-%02dT%02d:%02d:%02d.%06d",
             tm_result.tm_year, tm_result.tm_mon, tm_result.tm_mday,
             tm_result.tm_hour, tm_result.tm_min, tm_result.tm_sec,
             usec);
}
```
