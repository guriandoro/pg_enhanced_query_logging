# Improvement: No-op `poll_query_until` masquerades as a synchronisation wait

## Status: Fixed

## Priority: Low

## Effort: Trivial

## Description

In `t/001_basic_logging.pl`, the following code appears after running a
query and before checking that the log file exists:

```perl
$node->safe_psql('postgres', "SELECT 'hello_peql'");

# Give it a moment for the file to be written
$node->poll_query_until('postgres', "SELECT 1", '1');

ok(-f $log_file, "log file exists after running a query");
```

`poll_query_until('postgres', "SELECT 1", '1')` polls until `SELECT 1`
returns `'1'`, which is **always true on the first attempt**. This call
returns immediately and provides no actual waiting or synchronisation.

The test works correctly anyway because:
1. `safe_psql` is synchronous -- it waits for the query to complete.
2. The extension writes the log entry synchronously in `ExecutorEnd`,
   before returning the result to the client.
3. `fclose` in the C code flushes the stdio buffer before returning.

So by the time `safe_psql` returns, the log file is already written. The
`poll_query_until` call is dead code and the comment is misleading.

## Location

`t/001_basic_logging.pl`, lines 28-29

## Fix

Remove the no-op call and update the comment:

```perl
$node->safe_psql('postgres', "SELECT 'hello_peql'");
# safe_psql is synchronous and the extension writes in ExecutorEnd,
# so the log file exists by the time safe_psql returns.

ok(-f $log_file, "log file exists after running a query");
```
