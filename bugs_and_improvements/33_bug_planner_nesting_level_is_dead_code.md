# Bug: `planner_nesting_level` is maintained but never read

## Status: Fixed

## Priority: Medium

## Effort: Easy

## Description

Bug #04 introduced `planner_nesting_level` as a separate counter to avoid
conflating planner and executor nesting. The variable is correctly
incremented before calling the planner and decremented in `PG_FINALLY`:

```c
planner_nesting_level++;
PG_TRY();
{
    /* ... call prev_planner or standard_planner ... */
}
PG_FINALLY();
{
    planner_nesting_level--;
}
PG_END_TRY();

INSTR_TIME_SET_CURRENT(duration);
INSTR_TIME_SUBTRACT(duration, start);
peql_current_plan_time_ms = INSTR_TIME_GET_MILLISEC(duration);
```

However, `planner_nesting_level` is **never read** anywhere in the codebase.
The assignment to `peql_current_plan_time_ms` at line 589 is unconditional,
so every recursive planner call (subqueries, CTEs) overwrites the value.

For non-recursive planning this is harmless -- the single call sets the
correct timing. For recursive planning, the outermost call runs last (since
calls are synchronous and `start`/`duration` are stack-local), so the
outermost timing is what survives. This is arguably correct behaviour (total
plan time including sub-plans), but the unused variable suggests the original
intent was to guard the assignment with `if (planner_nesting_level == 0)` to
capture only the outermost call's timing.

## Location

`pg_enhanced_query_logging.c`, lines 166 (declaration), 571-589 (usage)

## Fix

**Option A**: Remove `planner_nesting_level` entirely if the current
behaviour (outermost timing naturally wins) is acceptable. This eliminates
dead code.

**Option B**: Add the guard that was likely intended:

```c
PG_END_TRY();

INSTR_TIME_SET_CURRENT(duration);
INSTR_TIME_SUBTRACT(duration, start);
if (planner_nesting_level == 0)
    peql_current_plan_time_ms = INSTR_TIME_GET_MILLISEC(duration);
```

This explicitly records only the top-level plan time and makes the variable's
purpose clear.
