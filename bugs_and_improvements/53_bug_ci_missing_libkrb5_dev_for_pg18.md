# Bug: CI missing `libkrb5-dev` for PG 18 GSSAPI header dependency

## Status: Fixed

## Priority: High

## Effort: Trivial

## Description

PostgreSQL 18's `libpq/libpq-be.h` includes `libpq/pg-gssapi.h`, which in
turn includes `<gssapi/gssapi.h>`. This system header is provided by the
`libkrb5-dev` package on Ubuntu. The CI workflow did not install this package,
causing the PG 18 / ubuntu-latest job to fail at compile time with:

```
fatal error: gssapi/gssapi.h: No such file or directory
```

This was not needed for PG 17 because the GSSAPI include path was made
non-optional in PG 18's server headers.

## Location

`.github/workflows/test.yml`, Ubuntu package install step

## Fix

Added `libkrb5-dev` to the `apt-get install` list:

```yaml
sudo apt-get install -y \
  postgresql-${{ matrix.pg_version }} \
  postgresql-server-dev-${{ matrix.pg_version }} \
  libkrb5-dev \
  libipc-run-perl
```
