# Bug: TAP test 005 parameter values not logged for PREPARE/EXECUTE

## Status: Fixed

## Priority: Medium

## Effort: Medium

## Description

TAP test `t/005_parameter_values.pl` fails 4 out of 5 subtests because the
`# Parameters:` line never appears in the log when using `PREPARE`/`EXECUTE`
via `safe_psql`.

The test runs:

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

- `t/005_parameter_values.pl` lines 11-34
- `pg_enhanced_query_logging.c`: parameter logging logic in `peql_format_entry`

## Fix

The test needs to use the **extended query protocol** to send actual bind
parameters. Options:

1. Use `$node->connect_ok` with a small Perl DBI/DBD::Pg script that calls
   `prepare` / `execute` with bind values.
2. Use pgbench with a custom script containing `\set` and `$1`-style parameters.
3. Use `psql`'s `\bind` meta-command (PG 16+) followed by `\g`:

```sql
PREPARE param_test (int, text) AS SELECT $1, $2;
\bind 42 'hello world'
\g
```

Option 3 is simplest if targeting PG 16+. Example fix:

```perl
my $content = reset_and_get_log($node, query_sql => q{
SET peql.log_parameter_values = on;
PREPARE param_test (int, text) AS SELECT \$1, \$2;
\bind 42 'hello world'
\g
DEALLOCATE param_test;
});
```
