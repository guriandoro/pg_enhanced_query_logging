# Bug: Utility entry `Schema` field uses database name instead of search_path

## Status: Fixed

## Priority: Low

## Effort: Easy

## Description

In `peql_format_entry` (the regular query formatter), the `Schema` field is
populated by inspecting the session's search path:

```c
PG_TRY();
{
    List   *search_path = fetch_search_path(false);
    if (search_path != NIL)
    {
        Oid     first_ns = linitial_oid(search_path);
        schema_name = get_namespace_name(first_ns);
        list_free(search_path);
    }
}
PG_CATCH();
{
    FlushErrorState();
    schema_name = NULL;
}
PG_END_TRY();
```

However, in `peql_format_utility_entry`, the `Schema` field is set to the
database name (`db`):

```c
appendStringInfo(buf, "# Thread_id: %d  Schema: %s  Last_errno: 0  Killed: 0\n",
                 MyProcPid, db);
```

This inconsistency means:
- Regular queries: `Schema: public` (or whatever the first search_path entry is)
- Utility statements: `Schema: postgres` (or whatever the database name is)

Consumers parsing the log (including pt-query-digest) will see the database
name in the Schema field for DDL statements, which is misleading. The Schema
field should have the same semantics for both entry types.

## Location

`pg_enhanced_query_logging.c`:
- Lines 1079-1102 (`peql_format_entry` -- correct implementation)
- Lines 1529-1531 (`peql_format_utility_entry` -- uses `db` instead)

## Fix

Extract the search_path lookup into a helper function and call it from both
formatters, or duplicate the `PG_TRY`/`fetch_search_path` block in
`peql_format_utility_entry`:

```c
const char *schema_name = NULL;

PG_TRY();
{
    List   *search_path = fetch_search_path(false);
    if (search_path != NIL)
    {
        Oid     first_ns = linitial_oid(search_path);
        schema_name = get_namespace_name(first_ns);
        list_free(search_path);
    }
}
PG_CATCH();
{
    FlushErrorState();
    schema_name = NULL;
}
PG_END_TRY();

appendStringInfo(buf, "# Thread_id: %d  Schema: %s  Last_errno: 0  Killed: 0\n",
                 MyProcPid,
                 schema_name ? schema_name : db);
```
