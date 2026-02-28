# Improvement: Extension marked `relocatable = true` but should not be

## Status: Not fixed

## Priority: Low

## Effort: Trivial

## Description

The control file declares:

```
relocatable = true
```

A relocatable extension can be moved to a different schema via
`ALTER EXTENSION ... SET SCHEMA`. However, the extension's SQL function is
defined without a schema-qualified name:

```sql
CREATE FUNCTION pg_enhanced_query_logging_reset()
```

If a user relocates the extension to a different schema, the function moves
with it. This is harmless for the function itself, but the C code contains a
`superuser()` check that is independent of schema. The issue is more
principled:

- The extension has no tables, types, or operators that benefit from
  relocation.
- The single function is a maintenance utility that belongs in `public`
  or an admin schema.
- Marking it `relocatable = true` is misleading and could cause confusion
  about where the function lives.

Most PostgreSQL extensions with a single C function and no data types use
`relocatable = false`.

## Location

`pg_enhanced_query_logging.control`, line 5

## Fix

Change to:

```
relocatable = false
```

This prevents accidental relocation and matches the convention of similar
extensions.
