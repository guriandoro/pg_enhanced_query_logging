# Improvement: `%zu` format specifier portability for JIT and memory metrics

## Status: Fixed

## Priority: Low

## Effort: Trivial

## Description

The code uses `%zu` for `size_t` values in two places:

1. `JIT_functions` (line ~1108): `ji.created_functions` is `size_t`
2. `Mem_allocated` (line ~1123): `MemoryContextMemAllocated()` returns `Size`

PostgreSQL cross-platform code avoids `%zu` because older MSVC versions and some
cross-compilation toolchains don't support it. The PostgreSQL convention is to cast
to a known-width type and use the corresponding format macro.

## Location

`pg_enhanced_query_logging.c`, lines ~1108 and ~1123

## Fix

Cast to `uint64` and use `UINT64_FORMAT`:

```c
appendStringInfo(buf, "# JIT_functions: " UINT64_FORMAT, (uint64) ji.created_functions);
appendStringInfo(buf, "# Mem_allocated: " UINT64_FORMAT "\n", (uint64) mem);
```
