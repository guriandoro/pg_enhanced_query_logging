# Bug: `PG_MODULE_MAGIC_EXT` macro breaks compilation on PostgreSQL 17

## Status: Fixed

## Priority: Critical

## Effort: Trivial

## Description

The extension uses `PG_MODULE_MAGIC_EXT(...)` to declare the module magic
block with a name and version. This macro was introduced in PostgreSQL 18;
PostgreSQL 17 only provides the plain `PG_MODULE_MAGIC` macro. On PG 17 the
build fails with:

```
error: implicit declaration of function 'PG_MODULE_MAGIC_EXT'
```

## Location

`pg_enhanced_query_logging.c`, module magic declaration (line 70-73)

## Fix

Added a version guard to use the extended macro on PG 18+ and fall back to
the classic macro on older versions:

```c
#if PG_VERSION_NUM >= 180000
PG_MODULE_MAGIC_EXT(
    .name = "pg_enhanced_query_logging",
    .version = "1.2"
);
#else
PG_MODULE_MAGIC;
#endif
```
