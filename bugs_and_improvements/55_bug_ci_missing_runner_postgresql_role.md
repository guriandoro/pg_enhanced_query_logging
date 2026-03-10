# Bug: CI missing PostgreSQL role for `runner` user

## Status: Fixed

## Priority: High

## Effort: Trivial

## Description

After fixing the port mismatch (report 54), the CI "Run pg_regress tests"
step failed with:

```
psql: error: connection to server on socket "/var/run/postgresql/.s.PGSQL.5432" failed: FATAL:  role "runner" does not exist
```

`pg_createcluster` creates a cluster owned by the `postgres` system user and
only bootstraps a `postgres` superuser role.  GitHub Actions runs steps as the
`runner` OS user, so `pg_regress` (which connects as the current OS user by
default) could not authenticate because no matching PostgreSQL role existed.

## Location

`.github/workflows/test.yml`, "Start PostgreSQL for installcheck" step

## Fix

Added `sudo -u postgres createuser -s "$USER"` immediately after starting the
cluster.  This creates a superuser role matching the current OS user (`runner`)
so that `pg_regress` can connect and manage the test database.

```yaml
sudo pg_ctlcluster ${{ matrix.pg_version }} test start
sudo -u postgres createuser -s "$USER"
```
