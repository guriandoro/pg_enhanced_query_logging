# Bug: `AllocateFile` may fail outside of a valid resource owner context

## Status: Fixed

## Priority: Medium

## Effort: Medium

## Description

`peql_flush_to_file` uses `AllocateFile`/`FreeFile` for managed file I/O:

```c
fp = AllocateFile(logpath, "a");
...
FreeFile(fp);
```

`AllocateFile` registers the file descriptor with the current `ResourceOwner`,
which is typically the transaction's resource owner. However, `peql_flush_to_file`
is called from `peql_ExecutorEnd`, which can be invoked in contexts where the
resource owner is in an unusual state:

1. **During transaction abort**: When a query fails, the executor cleanup path
   calls `ExecutorEnd`. The `ResourceOwner` may be in the process of being
   released. Registering a new file with a dying resource owner can cause an
   assertion failure in debug builds or a silent resource leak in production.

2. **During backend exit**: If a backend is terminating (e.g., `SIGTERM`),
   executor cleanup runs but resource owners may already be partially destroyed.

3. **Subtransaction edge case**: In a subtransaction that is being aborted,
   the subtransaction's resource owner is being cleaned up. `AllocateFile`
   registers with that owner, but `FreeFile` might run after the owner is
   already released, causing a "resource owner has no file" warning.

The `auto_explain` extension avoids this problem by using `elog(LOG, ...)` which
routes through PostgreSQL's built-in logging infrastructure, never opening files
directly from executor hooks.

## Location

`pg_enhanced_query_logging.c`, lines 1193-1232 (`peql_flush_to_file`)

## Fix

**Option A** (Recommended): Replace `AllocateFile`/`FreeFile` with raw
`fopen`/`fclose`. Since the file is opened, written, and closed within a single
function call (no need for resource tracking across function boundaries), managed
file handles provide no benefit here. Raw `fopen` is what the syslogger itself
uses for the PostgreSQL log.

**Option B** (Alternative): Before calling `AllocateFile`, check
`CurrentResourceOwner` is valid. If not, fall back to `fopen`/`fclose`.
