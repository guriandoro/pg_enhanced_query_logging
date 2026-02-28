# Improvement: Add `_PG_fini()` for clean hook uninstallation

## Status: Fixed

## Priority: Low

## Effort: Easy

## Description

The extension installs six hooks (planner, ExecutorStart/Run/Finish/End,
ProcessUtility) in `_PG_init()` but provides no `_PG_fini()` to restore the
previous hook pointers.

While PostgreSQL does not commonly unload extension shared libraries, having a
`_PG_fini()` is good practice:

- It enables safe use of `LOAD` / module reload during development and testing.
- If the shared library were ever unloaded without restoring hooks, the server
  would crash on the next query due to dangling function pointers.
- It documents the extension's cleanup contract.

## Location

`pg_enhanced_query_logging.c` (missing function)

## Fix

Add a `_PG_fini()` that restores all saved hook pointers:

```c
void
_PG_fini(void)
{
    planner_hook        = prev_planner;
    ExecutorStart_hook  = prev_ExecutorStart;
    ExecutorRun_hook    = prev_ExecutorRun;
    ExecutorFinish_hook = prev_ExecutorFinish;
    ExecutorEnd_hook    = prev_ExecutorEnd;
    ProcessUtility_hook = prev_ProcessUtility;
}
```
