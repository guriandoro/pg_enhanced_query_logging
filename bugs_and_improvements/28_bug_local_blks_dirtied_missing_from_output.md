# Bug: `Local_blks_dirtied` is missing from buffer output at full verbosity

## Status: Not fixed

## Priority: Low

## Effort: Trivial

## Description

The shared buffer line includes all four counters:

```c
"# Shared_blks_hit: " INT64_FORMAT
"  Shared_blks_read: " INT64_FORMAT
"  Shared_blks_dirtied: " INT64_FORMAT
"  Shared_blks_written: " INT64_FORMAT "\n",
bu->shared_blks_hit,
bu->shared_blks_read,
bu->shared_blks_dirtied,
bu->shared_blks_written);
```

But the local buffer line only has three:

```c
"# Local_blks_hit: " INT64_FORMAT
"  Local_blks_read: " INT64_FORMAT
"  Local_blks_written: " INT64_FORMAT "\n",
bu->local_blks_hit,
bu->local_blks_read,
bu->local_blks_written);
```

`local_blks_dirtied` is a valid field in `BufferUsage` and is tracked by
PostgreSQL. Its omission means local buffer dirty operations are invisible
in the log, creating an asymmetry with the shared buffer reporting.

The README field reference table also omits this field, so the code and
documentation are consistent with each other but inconsistent with what
PostgreSQL actually tracks.

## Location

`pg_enhanced_query_logging.c`, lines 1032-1038 (`peql_format_entry`)

## Fix

Add `Local_blks_dirtied` to the local buffer line:

```c
appendStringInfo(buf,
                 "# Local_blks_hit: " INT64_FORMAT
                 "  Local_blks_read: " INT64_FORMAT
                 "  Local_blks_dirtied: " INT64_FORMAT
                 "  Local_blks_written: " INT64_FORMAT "\n",
                 bu->local_blks_hit,
                 bu->local_blks_read,
                 bu->local_blks_dirtied,
                 bu->local_blks_written);
```

Also update the README field reference table to include this field.
