# Bug: Session rate limit decision is never recalculated when GUC changes

## Status: Not fixed

## Priority: Medium

## Effort: Easy

## Description

In session-mode rate limiting, the sampling decision is made once per backend
lifetime and cached:

```c
if (peql_rate_limit_type == PEQL_RATE_LIMIT_SESSION)
{
    if (!peql_session_decided)
    {
        uint32 r = (uint32) (pg_prng_uint64(&pg_global_prng_state)
                             % (uint64) peql_rate_limit);
        peql_session_is_sampled = (r == 0);
        peql_session_decided = true;
    }
    return peql_session_is_sampled;
}
```

Once `peql_session_decided` is set to `true`, it is never cleared. This means:

1. If a DBA changes `peql.rate_limit` (e.g., from 10 to 1) via `ALTER SYSTEM`
   and reloads, existing backends continue using the old sampling decision.
   A backend that was excluded under `rate_limit=10` stays excluded even after
   `rate_limit=1` (which should log everything).

2. If a DBA switches `peql.rate_limit_type` from `session` to `query` and back
   to `session`, the stale `peql_session_decided = true` means no new draw
   happens -- the backend keeps its old decision.

3. If `peql.rate_limit` is reduced from e.g. 1000 to 2, backends that were
   sampled under the old rate still show `Log_slow_rate_limit: 1000` in their
   output (actually, the current GUC value is logged, but the *decision* was
   made with the old value, creating inconsistency).

## Location

`pg_enhanced_query_logging.c`, lines 632-662 (`peql_should_log`)

## Fix

Add a GUC assign hook for `peql.rate_limit` and `peql.rate_limit_type` that
resets `peql_session_decided = false` whenever either value changes:

```c
static void
peql_rate_limit_assign(int newval, void *extra)
{
    peql_session_decided = false;
}
```

Register these hooks as the `assign_hook` parameter in the corresponding
`DefineCustomIntVariable` / `DefineCustomEnumVariable` calls.

Alternatively, store the `rate_limit` value that was used when the session
decision was made, and re-draw whenever the current GUC value differs from
the stored one.
