# Improvement: Fragile microsecond extraction from TimestampTz

## Status: Fixed

## Priority: Low

## Effort: Easy

## Description

The microsecond component of the timestamp is extracted with:

```c
usec = (int)(now % 1000000);
if (usec < 0) usec += 1000000;
```

`TimestampTz` is microseconds since 2000-01-01 (PostgreSQL epoch), not the Unix
epoch. The modulo happens to produce the correct fractional-second value because
both epochs are aligned on whole seconds, but this is non-obvious and fragile.

A future reader (or a platform with a different epoch base) could easily be misled.
The same pattern is duplicated in `peql_format_utility_entry` at lines 1326-1327.

## Location

`pg_enhanced_query_logging.c`, lines 924-925 and 1326-1327

## Fix

Use PostgreSQL's `timestamp2tm()` to decompose the `TimestampTz` into its
components in one call, which gives both the broken-down time fields and the
fractional second. This is what `log_line_prefix` and `timestamptz_to_str` use
internally. Alternatively, document the epoch-alignment assumption clearly.
