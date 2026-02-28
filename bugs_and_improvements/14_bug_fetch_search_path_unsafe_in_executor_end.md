# Bug: `fetch_search_path()` in ExecutorEnd can cause errors or catalog lock contention

## Status: Not fixed

## Priority: High

## Effort: Medium

## Description

At standard+ verbosity, `peql_format_entry` calls `fetch_search_path(false)` to
resolve the current schema name for the `Schema:` field:

```c
search_path = fetch_search_path(false);
if (search_path != NIL)
{
    Oid first_ns = linitial_oid(search_path);
    schema_name = get_namespace_name(first_ns);
    list_free(search_path);
}
```

This code runs from inside `peql_ExecutorEnd`, which is called during query
teardown. There are several problems:

1. **Catalog access during cleanup**: `get_namespace_name()` does a syscache
   lookup (on `pg_namespace`), which requires a valid transaction snapshot and
   can take catalog locks. During error recovery or aborted transactions,
   the snapshot may be invalid, causing secondary errors that mask the original
   failure.

2. **Lock contention**: Under high concurrency, the syscache lookup for every
   logged query adds lock contention on the `pg_namespace` catalog, potentially
   degrading performance -- precisely when the system is already slow enough
   to trigger logging.

3. **Error-within-error risk**: If the original query failed with an error,
   `ExecutorEnd` is called during cleanup. A secondary error from
   `get_namespace_name` (e.g., cache lookup failure) triggers a re-entrant
   error handler, which can result in a PANIC and server crash.

## Location

`pg_enhanced_query_logging.c`, lines 948-968 (`peql_format_entry`)

## Fix

Several approaches, from simplest to most robust:

**Option A** (Simple): Cache the schema name earlier. In `peql_ExecutorStart`,
resolve and store the schema name in a `static char *` variable. This runs at
query start when the transaction is fully valid.

**Option B** (Defensive): Wrap the `fetch_search_path` + `get_namespace_name`
calls in a `PG_TRY` block and fall back to just emitting the database name if
the lookup fails.

**Option C** (Safest): Use `namespace_search_path` (the raw GUC string) instead
of doing a catalog lookup. This gives the configured search path string directly
without any syscache access. Less precise (shows the setting, not the resolved
OID name), but zero risk.
