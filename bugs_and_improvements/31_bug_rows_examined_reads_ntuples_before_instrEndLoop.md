# Bug: `Rows_examined` always 0 because walker reads `ntuples` before `InstrEndLoop`

## Status: Fixed

## Priority: Critical

## Effort: Easy

## Description

The plan walker reads `planstate->instrument->ntuples` to accumulate
`Rows_examined`. However, `ntuples` is only populated when `InstrEndLoop()`
is called on a node's `Instrumentation` struct. During normal execution,
`InstrStopNode()` increments `tuplecount` (the running counter); only
`InstrEndLoop()` moves `tuplecount` into `ntuples` and resets it.

In `peql_ExecutorEnd`, `InstrEndLoop` is called only on
`queryDesc->totaltime` (the top-level timer). The per-node instrumentation
structs never have their loops ended before the plan walker runs, so:

- `ntuples` = 0 on every node (never finalised)
- `tuplecount` holds all the actual tuple counts

This causes `Rows_examined` to always report 0, even when a query scans
thousands of rows and `Full_scan: Yes` confirms a sequential scan was used.

From `instrument.c`:

```c
void InstrEndLoop(Instrumentation *instr)
{
    ...
    instr->ntuples += instr->tuplecount;  /* <-- moves running to final */
    instr->nloops  += 1;
    ...
    instr->tuplecount = 0;                /* <-- resets running counter */
}
```

## Location

`pg_enhanced_query_logging.c`, `peql_plan_walker()` — the line that reads
`planstate->instrument->ntuples`.

## Fix

Read both `ntuples` (already-finalised loops) and `tuplecount` (current
running counter that hasn't been finalised yet):

```c
if (planstate->instrument)
    m->rows_examined += planstate->instrument->ntuples
                      + planstate->instrument->tuplecount;
```

This correctly captures all tuples regardless of whether `InstrEndLoop`
has been called on the node.
