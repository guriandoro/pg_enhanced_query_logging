# Bug: PG 18-only `#include` headers break compilation on PostgreSQL 17

## Status: Fixed

## Priority: Critical

## Effort: Trivial

## Description

The source file unconditionally includes `commands/explain_format.h` and
`commands/explain_state.h`, which were split out from `commands/explain.h` in
PostgreSQL 18. These headers do not exist in PostgreSQL 17, so the extension
fails to compile with a fatal error:

```
fatal error: commands/explain_format.h: No such file or directory
```

All types and functions used by the extension (`ExplainState`,
`EXPLAIN_FORMAT_TEXT`, `ExplainBeginOutput`, etc.) are already provided by
`commands/explain.h` in PostgreSQL 17, so the two extra includes are only
needed on PG 18+.

## Location

`pg_enhanced_query_logging.c`, includes section (lines 35-36)

## Fix

Wrapped the two PG 18-only includes in a version guard:

```c
#include "commands/explain.h"
#if PG_VERSION_NUM >= 180000
#include "commands/explain_format.h"
#include "commands/explain_state.h"
#endif
```
