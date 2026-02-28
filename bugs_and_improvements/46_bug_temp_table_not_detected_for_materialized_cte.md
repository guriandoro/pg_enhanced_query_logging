# Bug: Temp_table not detected for MATERIALIZED CTEs

## Status: Fixed

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
# SET work_mem = '64kB';
# WITH big AS MATERIALIZED (SELECT generate_series(1, 10000) AS n)
#   SELECT count(*) FROM big;
like($content, qr/Temp_table: Yes/,
    "materialized CTE triggers Temp_table: Yes");
```

The test failed because the plan tree contained no `MaterialState` node, so
`Temp_table: No` was reported. Meanwhile `Temp_table_on_disk: Yes` was
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

- `pg_enhanced_query_logging.c`: `peql_plan_walker` line 950
- `t/010_edge_cases.pl` test 2

## Fix

Extended the plan tree walker to also detect `CteScanState` nodes as temp
table usage. A CTE scan always materializes its subquery results into a
tuplestore, which is conceptually equivalent to a temp table:

```c
if (IsA(planstate, MaterialState) || IsA(planstate, CteScanState))
{
    m->has_temp_table = true;
}
```

The test also sets `work_mem = '64kB'` and uses 10,000 rows (instead of
100,000) to keep the test fast while still exercising the CTE materialization
path.
