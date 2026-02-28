# Bug: `start` may be used uninitialized on error path in ProcessUtility hook

## Status: Not fixed

## Priority: Medium

## Effort: Trivial

## Description

In `peql_ProcessUtility`, the `do_log` flag is computed before executing the
utility statement, and `start` is only initialized when `do_log` is true:

```c
do_log = peql_log_utility && peql_enabled && peql_log_min_duration >= 0 &&
         (nesting_level == 0 || peql_log_nested);

if (do_log)
    INSTR_TIME_SET_CURRENT(start);

nesting_level++;
PG_TRY();
{
    ...standard_ProcessUtility...
}
PG_FINALLY();
{
    nesting_level--;
}
PG_END_TRY();

if (do_log)
{
    INSTR_TIME_SET_CURRENT(duration);
    INSTR_TIME_SUBTRACT(duration, start);
    ...
}
```

The issue: `do_log` is evaluated once at the start. If the utility statement
itself changes one of the GUC values that feed into `do_log` (e.g.,
`SET peql.log_utility = off` or `SET peql.enabled = off`), the second
`if (do_log)` check still fires because `do_log` was captured before the
SET statement ran.

This is not a crash risk (the `start` variable was initialized), but it means:
- A `SET peql.log_utility = off` statement can itself be logged to the slow
  query log, even though the user is trying to disable utility logging.
- A `SET peql.log_min_duration = '-1'` statement can itself be logged, even
  though the user is trying to disable all logging.

More significantly, the reverse is true: `SET peql.log_utility = on` will
**not** be logged, because `do_log` was computed when `log_utility` was still
off.

## Location

`pg_enhanced_query_logging.c`, lines 714-750 (`peql_ProcessUtility`)

## Fix

Re-evaluate the logging decision after the utility statement executes, or
accept this as a known behavior and document it. Most logging extensions
(including `auto_explain`) use the pre-execution GUC values, so this is
arguably consistent behavior.

If fixing, the simplest approach is to re-check the GUC values after execution:

```c
if (do_log)
{
    /* Re-check in case the utility statement changed our GUCs. */
    do_log = peql_log_utility && peql_enabled && peql_log_min_duration >= 0;
    ...
}
```
