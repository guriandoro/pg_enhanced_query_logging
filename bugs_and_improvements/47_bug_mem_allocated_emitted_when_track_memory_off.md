# Bug: Mem_allocated emitted when peql.track_memory = off

## Status: Open

## Priority: Medium

## Effort: Easy

## Description

TAP test `t/010_edge_cases.pl` test 4 ("track_memory=off: Mem_allocated field
absent") fails because `# Mem_allocated:` still appears in the log output
even after `SET peql.track_memory = off`.

The server-level config in `PeqlNode.pm` sets `peql.track_memory = on`.
The test then runs:

```sql
SET peql.track_memory = off;
SELECT generate_series(1, 100);
```

However, the log still contains `# Mem_allocated: ...` for the query. This
indicates the session-level `SET` either does not take effect for this GUC,
or the GUC check happens at a point where the session value has not yet
been applied.

### Possible causes

1. **GUC context issue**: `peql.track_memory` is defined as `PGC_SUSET`.
   The `SET` command in a superuser session should take effect immediately
   for subsequent queries. If it does not, the GUC may be read too early
   (e.g., cached at session start).

2. **Reset entry pollution**: The `reset_and_get_log()` helper first calls
   `pg_enhanced_query_logging_reset()` in a **separate session** where
   `track_memory = on` (the server default). That reset call's log entry
   includes `Mem_allocated`. The test then reads the entire log file
   (including the reset entry) and the `unlike` check matches the reset
   entry's `Mem_allocated` line.

   This is the most likely cause, as the test output shows two entries:
   the first (reset) with `Mem_allocated: 16384` and the second (the actual
   query) without it -- but the `unlike` matches the first entry.

### Test output

```
not ok 4 - track_memory=off: Mem_allocated field absent
# '# Time: ...
# ...
# # Mem_allocated: 16384
# SET timestamp=...;
# SELECT pg_enhanced_query_logging_reset();
# # Time: ...
# ...
# SET timestamp=...;
# SELECT generate_series(1, 100);
# '
#           matches '(?^:Mem_allocated:)'
```

## Location

- `t/010_edge_cases.pl` lines 42-49
- `t/PeqlNode.pm`: `reset_and_get_log()` function

## Fix

Same pattern as bug #43: the reset call is logged at the server-default
verbosity/settings. The test should either:

1. Filter out the reset entry before asserting, or
2. Issue the reset and the `SET peql.track_memory = off` in the same
   session so the reset is also logged without `Mem_allocated`:

```perl
$content = reset_and_get_log($node, query_sql => q{
SET peql.log_verbosity = 'full';
SET peql.track_memory = off;
SELECT generate_series(1, 100);
});

# Only check entries that are NOT the reset call
my @non_reset = grep { !/pg_enhanced_query_logging_reset/ }
    split(/(?=^# Time:)/m, $content);
unlike(join('', @non_reset), qr/Mem_allocated:/,
    "track_memory=off: Mem_allocated field absent");
```
