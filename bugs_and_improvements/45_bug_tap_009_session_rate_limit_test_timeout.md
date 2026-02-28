# Bug: TAP test 009 session rate limit test exits with code 60 (timeout)

## Status: Fixed

## Priority: Medium

## Effort: Easy

## Description

TAP test `t/009_session_rate_limit.pl` exits with code 60 and produces no
subtests. Exit code 60 from the PostgreSQL TAP test infrastructure typically
indicates a **timeout**.

The test runs 20 iterations of a loop. In each iteration it:

1. Calls `pg_enhanced_query_logging_reset()` via `safe_psql`
2. Sleeps 1 second
3. Runs 3 queries in a new `safe_psql` session
4. Sleeps 1 second
5. Reads the log file

With 20 iterations, each sleeping 2 seconds, the test takes at least
**40 seconds** of wall-clock time just on sleeps, plus overhead for each
`safe_psql` call. The default TAP test timeout (typically 60 seconds) is
exceeded.

### Test output

```
t/009_session_rate_limit.pl ..
Dubious, test returned 60 (wstat 15360, 0x3c00)
No subtests run
```

## Location

`t/009_session_rate_limit.pl`

## Fix

Reduced iterations from 20 to 4 and removed all `sleep` calls (the reset
function and `safe_psql` are synchronous -- the extension writes directly
via `open`/`write`/`close`, not through the logging collector). Also
refactored to use `PeqlNode.pm` helpers and added a second subtest
verifying that `rate_limit=1` with session mode logs everything:

```perl
my $sessions = 4;

for my $i (1 .. $sessions) {
    $node->safe_psql('postgres', "SELECT pg_enhanced_query_logging_reset()");

    $node->safe_psql('postgres', qq{
SELECT 'session_${i}_a';
SELECT 'session_${i}_b';
SELECT 'session_${i}_c';
});

    my $log_file = peql_log_path($node);
    my $content = '';
    if (-f $log_file) {
        eval { $content = slurp_file($log_file); };
    }
    # ... check all-or-nothing ...
}
```
