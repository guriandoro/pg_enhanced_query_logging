# Improvement: Add `Bytes_sent` field or remove from documentation

## Status: Not fixed

## Priority: Low

## Effort: Easy

## Description

The implementation plan and README list `Bytes_sent` as a standard-verbosity field:

> | `Bytes_sent` | Estimated from result set | standard+ |

However, the code never emits this field. This is a gap between the documented
output format and the actual output.

## Location

`pg_enhanced_query_logging.c`, `peql_format_entry()` (missing field)

## Fix

Either:

**Option A** - Emit an estimate. PostgreSQL doesn't track bytes sent to the client
at the executor level, but a rough approximation can be computed from the plan's
`plan_width * rows_processed`. This matches what pg_stat_statements does for
similar heuristics:

```c
if (peql_log_verbosity >= PEQL_LOG_VERBOSITY_STANDARD)
{
    int64 bytes_est = (int64) queryDesc->plannedstmt->planTree->plan_width
                    * (int64) rows_processed;
    appendStringInfo(buf, "# Bytes_sent: " INT64_FORMAT "\n", bytes_est);
}
```

**Option B** - Remove `Bytes_sent` from the README and implementation plan to
avoid documenting a field that doesn't exist.
