# Bug: `Rows_affected` logic misclassifies SELECT INTO and CTAS queries

## Status: Not fixed

## Priority: Low

## Effort: Easy

## Description

The code uses `queryDesc->operation` to decide between SELECT-style output
(with `Rows_sent`) and DML-style output (with `Rows_affected`):

```c
if (queryDesc->operation == CMD_SELECT)
{
    /* Rows_sent = es_processed */
}
else
{
    /* Rows_sent: 0 */
}

if (peql_log_verbosity >= PEQL_LOG_VERBOSITY_STANDARD &&
    queryDesc->operation != CMD_SELECT)
{
    appendStringInfo(buf, "# Rows_affected: " UINT64_FORMAT "\n",
                     rows_processed);
}
```

The operation type for `SELECT INTO` and `CREATE TABLE AS SELECT` is
`CMD_SELECT`, so these queries report `Rows_sent: N` (the number of rows
inserted into the new table) rather than `Rows_affected: N`. From a semantic
standpoint, these are DML-like operations that create rows, not return them
to the client. `Rows_sent` is misleading because no rows are actually sent
to the client.

Similarly, `INSERT ... RETURNING`, `UPDATE ... RETURNING`, and
`DELETE ... RETURNING` have operation types `CMD_INSERT`, `CMD_UPDATE`,
`CMD_DELETE` respectively, so they report `Rows_sent: 0` in the current code,
even though they do return rows to the client via the RETURNING clause.

## Location

`pg_enhanced_query_logging.c`, lines 977-1009 (`peql_format_entry`)

## Fix

For `CMD_SELECT`, check whether the query actually sends rows to the client
by examining `queryDesc->dest->mydest`. If the destination is
`DestIntoRel` (SELECT INTO) or `DestTransientRel` (CTAS), treat it as DML.

For RETURNING clauses, check `queryDesc->plannedstmt->hasReturning` and
report `Rows_sent` as `es_processed` even for DML operations.

Alternatively, this can be accepted as a known limitation and documented.
