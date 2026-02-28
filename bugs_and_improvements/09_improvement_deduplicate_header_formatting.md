# Improvement: Deduplicate header formatting between query and utility entries

## Status: Not fixed

## Priority: Low

## Effort: Easy

## Description

`peql_format_entry()` and `peql_format_utility_entry()` share approximately 40
lines of identical code for:

- Extracting user/host/db from `MyProcPort`
- Computing and formatting the ISO-8601 timestamp
- Emitting the `# Time:` line
- Emitting the `# User@Host:` line
- Emitting the `SET timestamp=` line
- Emitting rate limit metadata

Any bug fix to the header formatting (e.g., the microsecond extraction, the
`%ld` timestamp issue) must be applied in both places, which is error-prone.

## Location

`pg_enhanced_query_logging.c`, lines 873-1177 and 1296-1371

## Fix

Extract a shared helper function, e.g.:

```c
static void peql_format_header(StringInfo buf, double duration_ms,
                               uint64 rows_sent, double rows_examined,
                               uint64 rows_affected, bool is_select);
```

Both `peql_format_entry` and `peql_format_utility_entry` call this helper for the
common header, then append their type-specific content (buffer metrics, plan
quality, EXPLAIN output, etc.) afterward.
