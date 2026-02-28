# Bug: Plan time leaks to next query when current query isn't logged

## Status: Not fixed

## Priority: High

## Effort: Trivial

## Description

`peql_current_plan_time_ms` is only reset inside `peql_write_log_entry()`, which is
only called when a query exceeds the duration threshold and passes the rate limiter.

If query A is fast (below the threshold, so not logged), its planning time remains
in the static variable and gets incorrectly attributed to the *next* query B that
does get logged.

## Location

`pg_enhanced_query_logging.c`, lines 1382-1393 (`peql_write_log_entry`)

## Fix

Move the `peql_current_plan_time_ms = 0.0` reset into `peql_ExecutorEnd()`,
unconditionally, so it is cleared after every query regardless of whether a log
entry is written.
