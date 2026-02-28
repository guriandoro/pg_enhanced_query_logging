# Bug: Stale "known issue" in TESTING.md contradicts fixed code

## Status: Open

## Priority: Medium

## Effort: Trivial

## Description

`test/TESTING.md` lines 303-306 document a "known issue":

> **Known issue:** setting `rate_limit_always_log_duration = 0` currently does
> NOT trigger the always-log path due to a `> 0` guard in the code (see
> `bugs_and_improvements/21_*`). The query may still appear if it passes the
> normal rate limiter draw. Test will need updating after the fix.

However, the actual code in `peql_should_log` uses `>= 0`, not `> 0`:

```c
if (peql_rate_limit_always_log_duration >= 0 &&
    duration_ms >= peql_rate_limit_always_log_duration)
    return true;
```

The bug was fixed (the condition correctly allows `= 0` to mean "always log
any query"), but the documentation was never updated. The TAP test in
`t/002_rate_limiting.pl` (Test 5) already asserts the correct, fixed
behaviour.

This stale note misleads anyone reading the test documentation into thinking
the feature is broken when it is not.

## Location

`test/TESTING.md`, lines 303-306

## Fix

Remove or replace the "Known issue" note. For example:

```markdown
**Expected:** the `pg_sleep` query appears in the log despite the extreme
rate limit, because `always_log_duration = 0` means any query (duration >= 0)
bypasses the limiter.
```
