# Bug: nesting_level incremented in planner hook conflates plan vs. execution nesting

## Status: Fixed

## Priority: Medium

## Effort: Easy

## Description

The planner hook (`peql_planner`) increments `nesting_level` around the planner
call. However, `nesting_level` is also used in the executor hooks to determine
whether a statement is "nested" (inside a PL/pgSQL function, for example).

This conflation means that during planning of a top-level query, the nesting level
is > 0. If a function call during planning triggers executor activity, that
executor call sees `nesting_level > 0` and treats it as a nested statement. When
`peql.log_nested = false` (the default), such queries are silently suppressed
even though they should be logged.

## Location

`pg_enhanced_query_logging.c`, lines 508-521 (`peql_planner`)

## Fix

Use a separate counter (e.g., `planner_nesting_level`) for the planner hook, or
only increment `nesting_level` in the executor hooks (Run, Finish) where it is
semantically meaningful.
