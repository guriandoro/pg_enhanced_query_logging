# Bug: EXPLAIN output breaks pt-query-digest parsing

## Status: Fixed

## Priority: High

## Effort: Easy

## Description

When `peql.log_query_plan` is enabled, the EXPLAIN output is appended as:

```c
appendStringInfo(buf, "# Plan:\n# %s\n", es->str->data);
```

The `es->str->data` contains embedded newlines. Only the first line gets the `# `
prefix; all subsequent lines are bare text. pt-query-digest will interpret those
unprefixed lines as query text, corrupting the parsed output.

## Location

`pg_enhanced_query_logging.c`, line 1175 (`peql_format_entry`)

## Fix

After generating the EXPLAIN output, iterate through the string and prefix every
line with `# `. For example, replace each `\n` in the plan string with `\n# `
before appending, or append line-by-line in a loop.
