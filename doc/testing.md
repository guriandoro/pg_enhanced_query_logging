# Testing

[Back to README](../README.md)

The extension has three testing layers: SQL regression tests, Perl TAP (Test Anything Protocol) tests, and a manual testing guide. All automated tests use standard PostgreSQL testing infrastructure and work on both macOS and Linux.

## Prerequisites

Beyond the build requirements (C compiler, `pg_config`), the test suites need the PostgreSQL server development files and Perl modules that ship with them. The TAP tests additionally require `IPC::Run`, which is often not installed by default.

**Ubuntu / Debian:**

```bash
sudo apt-get install postgresql-server-dev-17 libipc-run-perl
```

Replace `17` with your PostgreSQL major version (e.g., `18`). The `postgresql-server-dev-*` package provides `pg_regress`, the `PostgreSQL::Test::Cluster` and `PostgreSQL::Test::Utils` Perl modules, and the PGXS (PostgreSQL Extension Build Infrastructure) Makefile infrastructure. `libipc-run-perl` provides the `IPC::Run` module required by the TAP harness.

**Fedora / RHEL / Rocky:**

```bash
sudo dnf install postgresql17-devel perl-IPC-Run
```

**macOS (Homebrew):**

```bash
brew install postgresql@17
```

Homebrew's `postgresql@*` formula includes the server dev files, `pg_regress`, and the Perl test modules.

`IPC::Run` is required for the TAP tests but is not included with the macOS system Perl. Install it with `cpan` (requires root) or `cpanm` (no root needed):

```bash
# Option A: cpan (installs to system Perl paths, requires sudo)
sudo cpan IPC::Run

# Option B: cpanm (installs to ~/perl5, no root required)
brew install cpanminus
cpanm IPC::Run
```

If you used `cpanm` (Option B), the module is installed under `~/perl5`. You must configure your shell to find it before running the TAP tests:

```bash
eval $(perl -I ~/perl5/lib/perl5/ -Mlocal::lib)
```

To make this permanent, add that line to your `~/.zshrc` (or `~/.bashrc`).

**macOS with PostgreSQL built from source:**

If you compiled PostgreSQL from source rather than using Homebrew, the TAP test Perl modules (`PostgreSQL::Test::Cluster`, `PostgreSQL::Test::Utils`) are in the source tree, not in the installed prefix. You need to:

1. **Match the source tree tag to the installed version.** For example, if you installed PostgreSQL 18.3, checkout `REL_18_3` in the source tree. A version mismatch causes API incompatibilities in the test Perl modules.

2. **Set the `PG_REGRESS` environment variable** to the `pg_regress` binary installed with PostgreSQL. The test Perl modules require this and will fail with cryptic errors if it is not set.

3. **Point `prove` at the source tree** with `-I`.

```bash
# Ensure source tree matches installed version
cd /path/to/postgres-source
git checkout REL_18_3  # match your installed version

# Set required environment variables
export PATH=/path/to/pg-install/bin:$PATH
export PG_REGRESS=/path/to/pg-install/lib/pgxs/src/test/regress/pg_regress

# Run TAP tests
prove -I /path/to/postgres-source/src/test/perl t/*.pl
```

**Verifying prerequisites:**

```bash
# Check that pg_config is available
pg_config --version

# Check that IPC::Run is installed (required for TAP tests)
perl -MIPC::Run -e 'print "ok\n"'

# Check that PostgreSQL test modules are available
perl -MPostgreSQL::Test::Cluster -e 'print "ok\n"'
```

If any of these fail, install the missing package as shown above. The SQL regression tests (`make installcheck`) only need `pg_regress` and do not require any Perl modules.

## Disabling Default PostgreSQL Logging

When testing this extension, you can disable PostgreSQL's built-in query logging so that only `pg_enhanced_query_logging` writes query entries.

Add these settings to `postgresql.conf` (or pass them via `ALTER SYSTEM` / `-c` flags) on your test instance:

```
log_statement = 'none'
log_min_duration_statement = -1
log_duration = off
```

- `log_statement = 'none'` prevents PostgreSQL from logging query text to its own log.
- `log_min_duration_statement = -1` disables PostgreSQL's built-in slow query logging (which is separate from `peql.log_min_duration`).
- `log_duration = off` prevents PostgreSQL from logging execution duration for every statement.

The automated TAP tests handle this automatically -- each test spins up a fresh `PostgreSQL::Test::Cluster` instance whose default configuration already has these settings off. If you are running manual tests against an existing server, verify these are disabled to avoid confusion between PostgreSQL's log output and the extension's `peql-slow.log`.

## Quick Start

Run the full automated test suite after building and installing:

```bash
# SQL regression tests (GUC defaults, extension load/unload)
make installcheck USE_PGXS=1

# TAP tests (log output, rate limiting, metrics, all features)
make prove_installcheck USE_PGXS=1
```

Or specify `pg_config` explicitly:

```bash
make installcheck USE_PGXS=1 PG_CONFIG=/path/to/pg_config
make prove_installcheck USE_PGXS=1 PG_CONFIG=/path/to/pg_config
```

**If you built PostgreSQL from source** and `make prove_installcheck` does
not work, run `prove` directly with the required environment:

```bash
export PATH=/path/to/pg-install/bin:$PATH
export PG_REGRESS=/path/to/pg-install/lib/pgxs/src/test/regress/pg_regress
prove -v -I /path/to/postgres-source/src/test/perl t/*.pl
```

See [Prerequisites](#prerequisites) for details on `IPC::Run`, source tree
version matching, and `PG_REGRESS`.

## SQL Regression Tests

Three tests in `sql/` validate core SQL behavior:

| Test | What it covers |
|------|----------------|
| `01_basic` | Extension CREATE/DROP, version check, reset function |
| `02_guc` | GUC defaults and validation for all `peql.*` settings |
| `03_filtering` | Statement filtering behavior |

Expected output files live in `expected/`. A failing test produces `regression.diffs` with the mismatch.

**Note:** The `02_guc` test checks GUC default values via `SHOW`. It expects the server to be running with default `peql.*` settings. If you have customized `peql.*` values in `postgresql.conf` (e.g., `peql.log_min_duration = 0`), the test will fail because the `SHOW` output won't match the expected defaults. Run `installcheck` against a server with no custom `peql.*` configuration, or use the TAP tests which spin up their own isolated instances.

## TAP Tests

Ten Perl TAP tests in `t/` verify actual log output using `PostgreSQL::Test::Cluster` to spin up temporary server instances. A shared helper module (`t/PeqlNode.pm`) provides common setup and log-reading utilities.

| Test | What it covers |
|------|----------------|
| `001_basic_logging` | Log file creation, pt-query-digest format, reset function, enable/disable |
| `002_rate_limiting` | Per-query sampling, rate-limit metadata, always-log-duration override |
| `003_extended_metrics` | Verbosity levels, buffer/WAL metrics, plan quality indicators, utility logging, row counts |
| `004_nested_logging` | `peql.log_nested` on/off with PL/pgSQL inner statements |
| `005_parameter_values` | Bind parameter logging, NULL handling, prepared statements |
| `006_query_plan` | EXPLAIN plan output in text and JSON format |
| `007_planning_time` | `peql.track_planning` on/off across verbosity levels |
| `008_min_duration` | Duration threshold filtering with `pg_sleep` |
| `009_session_rate_limit` | Session-mode rate limiting all-or-nothing behavior |
| `010_edge_cases` | Disk sort, disk temp table, memory tracking, schema field changes |

## Meson Build

The extension includes a `meson.build` for compatibility with PostgreSQL's Meson build system (PG 16+). When building as part of the PostgreSQL source tree:

```bash
meson setup build
cd build
meson test --suite pg_enhanced_query_logging
```

## CI/CD (Continuous Integration / Continuous Delivery)

A GitHub Actions workflow (`.github/workflows/test.yml`) runs both test suites automatically on every push and pull request. The CI matrix covers:

- **Platforms**: Ubuntu and macOS
- **PostgreSQL versions**: 17 and 18
- **Test suites**: pg_regress (SQL) and TAP (Perl)

Test artifacts (diffs, logs) are uploaded on failure for debugging.

## Sample Configuration

A sample configuration file is included at `pg_enhanced_query_logging.conf`. You can include it in `postgresql.conf`:

```
include = '/path/to/pg_enhanced_query_logging.conf'
```

## Manual Testing

See `test/TESTING.md` for a comprehensive manual testing guide covering all features with step-by-step instructions and expected output. The manual guide complements the automated tests and is useful for exploratory testing and validating output with `pt-query-digest`.
