# Bug: Unescaped parameter values can break log format

## Status: Not fixed

## Priority: High

## Effort: Easy

## Description

When `peql.log_parameter_values` is enabled, parameter values are emitted as:

```c
appendStringInfo(buf, "'%s'", val);
```

If a parameter value contains single quotes, newlines, or other special characters,
the log entry format is broken. A single quote inside a value produces malformed
output like `'it's a value'`. A newline inside a value creates a line break that
pt-query-digest will misparse as a separate log line.

## Location

`pg_enhanced_query_logging.c`, line 1279 (`peql_append_params`)

## Fix

Escape the value before appending. At minimum:
- Double single quotes (`'` -> `''`)
- Replace newlines with `\n` (literal backslash-n)
- Replace carriage returns with `\r`
