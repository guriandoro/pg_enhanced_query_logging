# Bug: TAP test reads potentially nonexistent file after reset

## Status: Fixed

## Priority: Low

## Effort: Easy

## Description

In `t/001_basic_logging.pl`, the test calls `pg_enhanced_query_logging_reset()`
and then immediately reads the log file:

```perl
$node->safe_psql('postgres', "SELECT pg_enhanced_query_logging_reset()");

my $after_reset = slurp_file($log_file);
unlike($after_reset, qr/hello_peql/,
    "log file no longer contains previous query after reset");
```

The reset function **renames** the file to `peql-slow.log.old` -- it does
not truncate in place. After the rename, the original `$log_file` path no
longer exists. A new file at that path is only created when the next query
is logged.

The behaviour of `slurp_file` on a nonexistent file depends on the
PostgreSQL test framework version:
- If it returns empty string: the `unlike` assertion passes vacuously
  (correct result, wrong reason).
- If it throws/dies: the test fails with a confusing Perl error rather than
  a clean TAP failure.

The test comment says "the file should exist but the previous content gone",
which is factually incorrect -- the file does not exist after reset.

## Location

`t/001_basic_logging.pl`, lines 57-64

## Fix

Either check for file existence explicitly, or run a query after reset to
ensure the file is recreated before reading it:

```perl
$node->safe_psql('postgres', "SELECT pg_enhanced_query_logging_reset()");

# Run a query to recreate the log file after reset renamed it.
$node->safe_psql('postgres', "SELECT 'post_reset_marker'");

my $after_reset = slurp_file($log_file);
unlike($after_reset, qr/hello_peql/,
    "log file no longer contains previous query after reset");
like($after_reset, qr/post_reset_marker/,
    "new log file contains queries after reset");
```

Alternatively, test that the file does not exist or is empty:

```perl
ok(! -f $log_file || -z $log_file,
    "log file absent or empty after reset");
```
