# Bug: `expected/01_basic.out` hardcodes version 1.0 but control file defaults to 1.1

## Status: Fixed

## Priority: Critical

## Effort: Trivial

## Description

The `.control` file sets `default_version = '1.1'`, so `CREATE EXTENSION
pg_enhanced_query_logging` installs version 1.1. However, the expected
regression test output hardcodes version `1.0`:

```
 pg_enhanced_query_logging | 1.0
```

This causes the `01_basic` regression test to **always fail** with a diff on
the `extversion` column.

## Location

`expected/01_basic.out`, line 9

## Fix

Change the expected version from `1.0` to `1.1`:

```
 pg_enhanced_query_logging | 1.1
```
