# Bug: EXECUTE of prepared statements not logged with parameter values

## Status: Fixed

## Priority: High

## Effort: Easy

## Description

When `peql.log_parameter_values = on`, executing a prepared statement via
SQL `EXECUTE` does not produce a `# Parameters:` line in the log. The
inner query (e.g., `SELECT $1, $2`) is not logged at all — only the
`EXECUTE` wrapper appears (and only if `peql.log_utility = on`).

Reproducer:

```sql
SET peql.log_parameter_values = on;
PREPARE param_test (int, text) AS SELECT $1, $2;
EXECUTE param_test(42, 'hello world');
DEALLOCATE param_test;
```

Expected: a log entry for the `SELECT $1, $2` query with
`# Parameters: $1 = '42', $2 = 'hello world'` appended.

Actual: no entry for the inner query; `grep "Parameters" peql-slow.log`
returns 0 matches.

### Root cause

`EXECUTE` is a utility statement, so PostgreSQL routes it through the
`ProcessUtility` hook. The extension's `peql_ProcessUtility` increments
`nesting_level` to 1 before calling the standard handler. Internally,
`ExecuteQuery()` evaluates the parameter expressions, builds a
`ParamListInfo`, creates a portal, and invokes the executor. This means
the executor hooks (`ExecutorStart`, `ExecutorRun`, `ExecutorEnd`) fire
for the real query with `queryDesc->params` properly populated.

However, the `peql_active()` macro requires `nesting_level == 0` (unless
`peql.log_nested = on`):

```c
#define peql_active() \
    (peql_enabled && peql_log_min_duration >= 0 && \
     (nesting_level == 0 || peql_log_nested))
```

Since `nesting_level` was 1 (bumped by `peql_ProcessUtility`), the
executor hooks saw `peql_active()` as false and skipped both
instrumentation setup (`ExecutorStart`) and log entry writing
(`ExecutorEnd`). The parameter values in `queryDesc->params` were never
formatted.

### Difference from bug #44

Bug #44 documented a TAP test failure and worked around it by switching
the test to use the extended query protocol (`\bind` / `\g`). That fix
was correct for testing parameter logging via libpq bind parameters, but
it did not address the underlying C-level issue: SQL-level
`PREPARE`/`EXECUTE` statements were silently dropped by the extension.

## Location

- `pg_enhanced_query_logging.c`: `peql_ProcessUtility()` function

## Fix

In `peql_ProcessUtility`, detect when the utility statement is an
`ExecuteStmt` (the node type for SQL `EXECUTE`) and skip the
`nesting_level` increment for that case. `EXECUTE` is a thin dispatch
wrapper — the real query execution happens inside the executor, and it
should be treated as a top-level call so the executor hooks can log it
with its bound parameter values.

```c
bool is_execute = IsA(pstmt->utilityStmt, ExecuteStmt);

if (!is_execute)
    nesting_level++;
PG_TRY();
{
    /* call prev or standard ProcessUtility */
}
PG_FINALLY();
{
    if (!is_execute)
        nesting_level--;
}
PG_END_TRY();
```

When `peql.log_utility = on`, the `EXECUTE` wrapper is still logged as a
utility entry (without parameters, since the utility hook does not have
access to the evaluated `ParamListInfo`). The inner executor entry now
also appears with the full parameter values. When `peql.log_utility = off`
(the default), only the executor entry appears.
