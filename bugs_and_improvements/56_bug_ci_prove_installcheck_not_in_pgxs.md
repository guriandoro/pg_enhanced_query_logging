# Bug: CI calls `prove_installcheck` which does not exist in PGXS

## Status: Fixed

## Priority: High

## Effort: Trivial

## Description

The CI workflow had a separate "Run TAP tests" step that invoked
`make prove_installcheck USE_PGXS=1`.  This target does not exist in
`pgxs.mk`; it is only defined in the in-tree `contrib-global.mk`.

When building with `USE_PGXS=1`, the PGXS `installcheck` target already
runs both `pg_regress` tests and TAP tests (via `$(prove_installcheck)`)
whenever `TAP_TESTS = 1` is set in the Makefile.  The separate step was
therefore both redundant and broken:

```
make: *** No rule to make target 'prove_installcheck'.  Stop.
```

## Location

`.github/workflows/test.yml`, "Run TAP tests" step

## Fix

Removed the separate "Run TAP tests" step.  The single `make installcheck
USE_PGXS=1` step already covers both pg_regress and TAP tests.  Renamed the
remaining step to "Run tests (pg_regress + TAP)" for clarity.
