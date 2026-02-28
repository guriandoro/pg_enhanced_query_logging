# Bug: Buffer and WAL metrics report cumulative totals, not per-query deltas

## Status: Fixed

## Priority: High

## Effort: Medium

## Description

The extension reads buffer and WAL usage directly from the `Instrumentation`
struct's `bufusage` and `walusage` fields:

```c
BufferUsage *bu = &instr->bufusage;
WalUsage    *wu = &instr->walusage;

appendStringInfo(buf,
                 "# Shared_blks_hit: " INT64_FORMAT ...
                 bu->shared_blks_hit, ...);
```

The implementation plan specifies these should be **deltas** (per-query values),
and the architecture diagram says:

> BufferUsage delta tracking (shared/local/temp blocks)
> WalUsage delta tracking

However, the `Instrumentation.bufusage` / `.walusage` fields populated by
`InstrEndLoop` contain the *total* counters accumulated across all loops of the
node (for `queryDesc->totaltime`, this means the entire query execution). Whether
this represents a true per-query delta depends on how `InstrAlloc` initializes
the counters.

When `InstrAlloc` creates the instrumentation, `bufusage` and `walusage` are
zero-initialized. `InstrStartNode` snapshots the *global* `pgBufferUsage` at
start, and `InstrEndLoop` computes `current_global - snapshot_at_start` and
adds it to the instrumentation's running total. So for a single-pass query,
the `bufusage` in `totaltime` does represent this query's delta.

**However**, there is still a correctness problem: `peql_ExecutorStart` installs
instrumentation *after* calling the standard executor start:

```c
if (prev_ExecutorStart)
    prev_ExecutorStart(queryDesc, eflags);
else
    standard_ExecutorStart(queryDesc, eflags);

/* Then allocate instrumentation */
if (queryDesc->totaltime == NULL)
    queryDesc->totaltime = InstrAlloc(...);
```

The instrumentation is allocated after `standard_ExecutorStart`, which means
`InstrStartNode` is never called for the "start" phase of query execution. The
actual start-time snapshot happens only when `ExecutorRun` first calls into
the plan tree. Any buffer I/O that happens during `ExecutorStart` (e.g., catalog
lookups for plan initialization) is therefore not captured.

Compare with `auto_explain` which also allocates totaltime in ExecutorStart
-- this is the standard pattern and the buffer I/O during ExecutorStart is
typically minimal. But it is a semantic deviation from "per-query delta."

## Location

`pg_enhanced_query_logging.c`, lines 539-564 (`peql_ExecutorStart`) and
lines 1016-1066 (`peql_format_entry`, buffer/WAL output)

## Fix

This is a known limitation shared with `auto_explain` and `pg_stat_statements`.
The practical impact is minimal since ExecutorStart rarely does significant I/O.

For documentation accuracy:
1. Update the README/plan to say "per-query executor deltas" rather than
   implying complete per-query deltas.
2. Alternatively, snapshot `pgBufferUsage` / `pgWalUsage` in ExecutorStart
   *before* chaining to the standard function, then compute the delta manually
   in ExecutorEnd. This would capture the full query lifecycle including start.
