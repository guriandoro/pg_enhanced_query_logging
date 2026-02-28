# Bug: Temp_table not detected for MATERIALIZED CTEs

## Status: Open

## Priority: Medium

## Effort: Medium

## Description

The plan tree walker sets `has_temp_table = true` only when it encounters a
`MaterialState` node (node type `T_MaterialState`). However, a
`WITH ... AS MATERIALIZED (...)` CTE does not produce a `MaterialState` node
in the plan tree. Instead, PostgreSQL uses a `CteScan` node that reads from a
`Tuplestorestate` which may spill to disk.

TAP test `t/010_edge_cases.pl` test 2 verifies:

```perl
# WITH big AS MATERIALIZED (SELECT generate_series(1, 100000) AS n)
#   SELECT count(*) FROM big;
like($content, qr/Temp_table: Yes/,
    "materialized CTE triggers Temp_table: Yes");
```

The test fails because the plan tree contains no `MaterialState` node, so
`Temp_table: No` is reported. Meanwhile `Temp_table_on_disk: Yes` is
correctly detected because that flag is derived from `temp_blks_written > 0`
(buffer-level accounting), not from plan node inspection.

### Test output

```
not ok 2 - materialized CTE triggers Temp_table: Yes
# ...
# # Full_scan: No  Temp_table: No  Temp_table_on_disk: Yes  Filesort: No
# ...
```

## Location

- `pg_enhanced_query_logging.c`: `peql_plan_walker` around line 950
- `t/010_edge_cases.pl` line 29

## Fix

Extend the plan tree walker to also detect `CteScanState` nodes as temp
table usage. A CTE scan always materializes its subquery results into a
tuplestore, which is conceptually equivalent to a temp table:

```c
if (IsA(planstate, MaterialState) || IsA(planstate, CteScanState))
{
    m->has_temp_table = true;
}
```

Alternatively, the test could be adjusted to use a query that genuinely
produces a `MaterialState` node, such as a subquery with `DISTINCT` or
a nested loop with a materializing inner side. However, detecting CTE
materialization is arguably the correct behavior for `Temp_table`.
