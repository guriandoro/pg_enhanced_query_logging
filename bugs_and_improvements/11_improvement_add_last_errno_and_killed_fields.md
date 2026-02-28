# Improvement: Add `Last_errno` and `Killed` fields per the output spec

## Status: Not fixed

## Priority: Low

## Effort: Trivial

## Description

The implementation plan specifies the standard-verbosity `# Thread_id:` line as:

```
# Thread_id: 12345  Schema: mydb.public  Last_errno: 0  Killed: 0
```

However, the actual code only emits `Thread_id` and `Schema`:

```c
appendStringInfo(buf, "# Thread_id: %d  Schema: %s.%s\n",
                 MyProcPid, db, schema_name);
```

`Last_errno` and `Killed` are MySQL-specific fields that are always `0` in
PostgreSQL, but including them improves compatibility with tools that parse the
full MySQL slow query log format.

## Location

`pg_enhanced_query_logging.c`, lines 963-967 (`peql_format_entry`)
and line 1344 (`peql_format_utility_entry`)

## Fix

Append `Last_errno: 0  Killed: 0` to the Thread_id/Schema line:

```c
appendStringInfo(buf, "# Thread_id: %d  Schema: %s.%s  Last_errno: 0  Killed: 0\n",
                 MyProcPid, db, schema_name);
```
