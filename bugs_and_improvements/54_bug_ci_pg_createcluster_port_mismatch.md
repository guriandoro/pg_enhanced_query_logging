# Bug: CI `pg_createcluster` does not pin port, causing connection failure

## Status: Fixed

## Priority: High

## Effort: Trivial

## Description

The "Start PostgreSQL for installcheck" CI step on Ubuntu creates a new
cluster named `test` after dropping the default `main` cluster.  However,
`pg_createcluster` was called without the `-p` flag, so it assigned the next
available port (typically 5433) instead of 5432.  `pg_regress` (invoked by
`make installcheck`) defaults to port 5432, so it tried to connect to a
non-existent Unix socket:

```
psql: error: connection to server on socket "/var/run/postgresql/.s.PGSQL.5432" failed: No such file or directory
	Is the server running locally and accepting connections on that socket?
```

The `PGPORT` environment variable was also not exported to subsequent CI
steps, so even if the cluster had started on 5432, downstream steps had no
guarantee of using the correct port.

## Location

`.github/workflows/test.yml`, "Start PostgreSQL for installcheck" step

## Fix

1. Added `-p 5432` to the `pg_createcluster` invocation to explicitly bind the
   cluster to port 5432.
2. Exported `PGPORT=5432` via `$GITHUB_ENV` so all subsequent steps use the
   correct port.

```yaml
sudo pg_createcluster ${{ matrix.pg_version }} test -p 5432 -- \
  -A trust
# ...
echo "PGHOST=/var/run/postgresql" >> "$GITHUB_ENV"
echo "PGPORT=5432" >> "$GITHUB_ENV"
```
