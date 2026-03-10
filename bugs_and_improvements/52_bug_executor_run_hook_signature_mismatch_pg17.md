# Bug: `ExecutorRun` hook signature mismatch on PostgreSQL 17

## Status: Fixed

## Priority: Critical

## Effort: Low

## Description

PostgreSQL 18 removed the `bool execute_once` parameter from the
`ExecutorRun` hook signature, reducing it from four to three arguments:

```c
/* PG 17 */
typedef void (*ExecutorRun_hook_type)(QueryDesc *, ScanDirection, uint64, bool);

/* PG 18 */
typedef void (*ExecutorRun_hook_type)(QueryDesc *, ScanDirection, uint64);
```

The extension declared `peql_ExecutorRun` with the PG 18 (3-argument)
signature only. On PG 17 this produced an incompatible-pointer-type warning
on hook assignment and "too few arguments" errors when calling
`prev_ExecutorRun` and `standard_ExecutorRun`:

```
error: too few arguments to function 'standard_ExecutorRun'
```

## Location

`pg_enhanced_query_logging.c`:
- Forward declaration of `peql_ExecutorRun`
- Function definition of `peql_ExecutorRun`
- Call sites for `prev_ExecutorRun` and `standard_ExecutorRun`

## Fix

Added `#if PG_VERSION_NUM < 180000` guards to include the `bool execute_once`
parameter in the declaration, definition, and all call sites when building
against PostgreSQL 17:

```c
static void
peql_ExecutorRun(QueryDesc *queryDesc, ScanDirection direction, uint64 count
#if PG_VERSION_NUM < 180000
                , bool execute_once
#endif
                )
{
    ...
    if (prev_ExecutorRun)
#if PG_VERSION_NUM >= 180000
        prev_ExecutorRun(queryDesc, direction, count);
#else
        prev_ExecutorRun(queryDesc, direction, count, execute_once);
#endif
    else
#if PG_VERSION_NUM >= 180000
        standard_ExecutorRun(queryDesc, direction, count);
#else
        standard_ExecutorRun(queryDesc, direction, count, execute_once);
#endif
}
```
