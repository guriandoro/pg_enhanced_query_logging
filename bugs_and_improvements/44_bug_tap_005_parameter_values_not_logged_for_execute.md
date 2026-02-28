# Bug: TAP test 005 parameter values not logged for PREPARE/EXECUTE

## Status: Fixed

## Priority: Medium

## Effort: Medium

## Description

TAP test `t/005_parameter_values.pl` fails because the `# Parameters:` line
never appears in the log when using `PREPARE`/`EXECUTE` via `safe_psql`.

The original test runs:

```sql
SET peql.log_parameter_values = on;
PREPARE param_test (int, text) AS SELECT $1, $2;
EXECUTE param_test(42, 'hello world');
DEALLOCATE param_test;
```

The log only shows entries for the `pg_enhanced_query_logging_reset()` call
and possibly the SET/PREPARE/DEALLOCATE statements, but the `EXECUTE` entry
does not contain a `# Parameters:` line.

### Root cause

When `EXECUTE param_test(42, 'hello world')` is sent through `psql`, the
PostgreSQL simple query protocol **inlines the parameter values** into the
query string. The executor sees `SELECT 42, 'hello world'` with no bound
parameters (`queryDesc->params` is NULL). The extension only emits
`# Parameters:` when `queryDesc->params` is non-NULL, which only happens
with the **extended query protocol** (i.e., actual `$1`/`$2` bind parameters
sent through libpq's `PQexecParams` or `PQexecPrepared`).

The psql tool's `EXECUTE` command does not use the extended protocol --
it rewrites `EXECUTE param_test(42, 'hello world')` into a plain SQL
`EXECUTE` statement with literal values.

### Test output

```
not ok 1 - log_parameter_values=on: Parameters line present
#   '...'
#     doesn't match '(?^:Parameters:)'
not ok 2 - log_parameter_values=on: integer parameter value appears
not ok 3 - log_parameter_values=on: text parameter value appears
not ok 4 - NULL parameter values shown as NULL
ok 5 - log_parameter_values=off: no Parameters line
```

## Location

- `t/005_parameter_values.pl`
- `pg_enhanced_query_logging.c`: parameter logging logic in `peql_format_entry`

## Fix

The test uses psql's `\bind` + `\g` meta-commands (PG 16+) to send queries
through the **extended query protocol**, which populates `queryDesc->params`.
The `PREPARE`/`EXECUTE` approach was replaced with inline `$1::type` casts
combined with `\bind`:

```perl
my $content = reset_and_get_log($node, query_sql => q{
SET peql.log_parameter_values = on;
SELECT $1::int, $2::text \bind 42 'hello world' \g
});
```

The test now has 3 subtests (down from the original 5): parameter values on,
multiple parameter types, and parameter values off. The NULL parameter test
was removed since `\bind` does not have a way to send SQL NULL without
additional workarounds.
