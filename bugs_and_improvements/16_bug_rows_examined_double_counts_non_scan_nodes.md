# Bug: `rows_examined` double-counts tuples by summing all nodes, not just scan nodes

## Status: Not fixed

## Priority: Medium

## Effort: Easy

## Description

The plan walker accumulates `rows_examined` from *every* instrumented node:

```c
if (planstate->instrument)
    m->rows_examined += planstate->instrument->ntuples;
```

The `ntuples` field on `Instrumentation` represents the number of tuples
*emitted* by that node. For a plan like:

```
Sort
  -> Hash Join
       -> Seq Scan on orders (1000 rows)
       -> Hash
            -> Seq Scan on customers (500 rows)
```

The walker sums ntuples from Sort + Hash Join + Seq Scan (orders) + Hash +
Seq Scan (customers) = a significantly inflated value. The MySQL `Rows_examined`
metric counts only the number of rows *read from storage* (i.e., scan nodes
only).

## Location

`pg_enhanced_query_logging.c`, lines 806-808 (`peql_plan_walker`)

## Fix

Only accumulate `ntuples` from actual scan nodes (leaf nodes that read from
tables or indexes):

```c
switch (nodeTag(planstate))
{
    case T_SeqScanState:
    case T_IndexScanState:
    case T_IndexOnlyScanState:
    case T_BitmapHeapScanState:
    case T_TidScanState:
    case T_TidRangeScanState:
    case T_ForeignScanState:
    case T_CustomScanState:
    case T_SampleScanState:
        if (planstate->instrument)
            m->rows_examined += planstate->instrument->ntuples;
        break;
    default:
        break;
}
```

This gives a semantically correct `Rows_examined` that matches what MySQL
reports and what pt-query-digest users expect.
